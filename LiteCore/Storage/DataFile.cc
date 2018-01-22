//
//  DataFile.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/12/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "DataFile.hh"
#include "Record.hh"
#include "DocumentKeys.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "Endian.hh"
#include "RefCounted.hh"
#include "c4Private.h"
#include "PlatformIO.hh"
#include <errno.h>
#include <mutex>              // std::mutex, std::unique_lock
#include <condition_variable> // std::condition_variable
#include <unordered_map>
#include <dirent.h>
#include <algorithm>

#include "SQLiteDataFile.hh"

using namespace std;


namespace litecore {

    LogDomain DBLog("DB");


#pragma mark - FACTORY:


    void DataFile::Factory::moveFile(const FilePath &from, const FilePath &to) {
        auto fromBaseLen = from.fileName().size();
        from.forEachMatch([&](const FilePath &f) {
            string toFile = to.fileName() + from.fileName().substr(fromBaseLen);
            f.moveTo(to.dirName() + toFile);
        });
    }

    bool DataFile::Factory::fileExists(const FilePath &path) {
        return path.exists();
    }


    std::vector<DataFile::Factory*> DataFile::factories() {
        return {&SQLiteDataFile::factory()};
    }


    DataFile::Factory* DataFile::factoryNamed(const std::string &name) {
        auto facs = factories();
        if (name.empty())
            return facs[0];
        for (auto factory : facs)
            if (name == factory->name())
                return factory;
        return nullptr;
    }

    DataFile::Factory* DataFile::factoryNamed(const char *name) {
        if (!name)
            name = "";
        return factoryNamed(string(name));
    }

    DataFile::Factory* DataFile::factoryForFile(const FilePath &path) {
        auto ext = path.extension();
        for (auto factory : factories())
            if (ext == factory->filenameExtension())
                return factory;
        return nullptr;
    }



#pragma mark - SHARED:


    /** Shared state between all open DataFile instances on the same filesystem file.
        Manages a mutex that ensures that only one DataFile can open a transaction at once. */
    class DataFile::Shared : public RefCounted, C4InstanceCounted {
    public:

        static Shared* forPath(const FilePath &path, DataFile *dataFile) {
            unique_lock<mutex> lock(sFileMapMutex);
            auto pathStr = path.path();
            Shared* file = sFileMap[pathStr];
            if (!file) {
                file = new Shared(path);
                sFileMap[pathStr] = file;
                LogToAt(DBLog, Debug, "File %p: created for DataFile %p at %s", file, dataFile, path.path().c_str());
            } else {
                LogToAt(DBLog, Debug, "File %p: adding DataFile %p", file, dataFile);
            }
            lock.unlock();

            file->addDataFile(dataFile);
            return file;
        }


        static size_t openCountOnPath(const FilePath &path) {
            unique_lock<mutex> lock(sFileMapMutex);
            auto pathStr = path.path();
            Shared* file = sFileMap[pathStr];
            return file ? file->openCount() : 0;
        }


        const FilePath path;                            // The filesystem path
        atomic<bool> isCompacting {false};              // Is the database compacting?


        Transaction* transaction() {
            return _transaction;
        }

        void addDataFile(DataFile *dataFile) {
            unique_lock<mutex> lock(_mutex);
            if (find(_dataFiles.begin(), _dataFiles.end(), dataFile) == _dataFiles.end())
                _dataFiles.push_back(dataFile);
        }

        bool removeDataFile(DataFile *dataFile) {
            unique_lock<mutex> lock(_mutex);
            LogToAt(DBLog, Debug, "File %p: Remove DataFile %p", this, dataFile);
            auto pos = find(_dataFiles.begin(), _dataFiles.end(), dataFile);
            if (pos == _dataFiles.end())
                return false;
            _dataFiles.erase(pos);
            return true;
        }


        void forOpenDataFiles(DataFile *except, function_ref<void(DataFile*)> fn) {
            unique_lock<mutex> lock(_mutex);
            for (auto df : _dataFiles)
                if (df != except)
                    fn(df);
        }


        size_t openCount() {
            unique_lock<mutex> lock(_mutex);
            return _dataFiles.size();
        }


        void setTransaction(Transaction* t) {
            Assert(t);
            unique_lock<mutex> lock(_transactionMutex);
            while (_transaction != nullptr)
                _transactionCond.wait(lock);
            _transaction = t;
        }


        void unsetTransaction(Transaction* t) {
            unique_lock<mutex> lock(_transactionMutex);
            Assert(t && _transaction == t);
            _transaction = nullptr;
            _transactionCond.notify_one();
        }


        Retained<RefCounted> sharedObject(const string &key) {
            lock_guard<mutex> lock(_mutex);
            auto i = _sharedObjects.find(key);
            if (i == _sharedObjects.end())
                return nullptr;
            return i->second;
        }


        Retained<RefCounted>  addSharedObject(const string &key, Retained<RefCounted> object) {
            lock_guard<mutex> lock(_mutex);
            auto e = _sharedObjects.emplace(key, object);
            return e.first->second;
        }


    protected:
        Shared(const FilePath &p)
        :path(p)
        { }

        ~Shared() {
            LogToAt(DBLog, Debug, "File %p: destructing", this);
            unique_lock<mutex> lock(sFileMapMutex);
            sFileMap.erase(path.path());
        }


    private:
        mutex              _transactionMutex;       // Mutex for transactions
        condition_variable _transactionCond;        // For waiting on the mutex
        Transaction*       _transaction {nullptr};  // Currently active Transaction object
        vector<DataFile*>  _dataFiles;              // Open DataFiles on this File
        unordered_map<string, Retained<RefCounted>> _sharedObjects;
        mutex              _mutex;                  // Mutex for _dataFiles and _sharedObjects

        static unordered_map<string, Shared*> sFileMap;
        static mutex sFileMapMutex;
    };


    unordered_map<string, DataFile::Shared*> DataFile::Shared::sFileMap;
    mutex DataFile::Shared::sFileMapMutex;


    size_t DataFile::Factory::openCount(const FilePath &path) {
        return Shared::openCountOnPath(path);
    }

    
#pragma mark - DATAFILE:


    const DataFile::Options DataFile::Options::defaults = DataFile::Options {
        {true},
        true, true
    };


    DataFile::DataFile(const FilePath &path, const DataFile::Options *options)
    :_shared(Shared::forPath(path, this)),
     _options(options ? *options : Options::defaults)
    { }

    DataFile::~DataFile() {
        LogToAt(DBLog, Debug, "DataFile: destructing (~DataFile)");
        Assert(!_inTransaction);
        _shared->removeDataFile(this);
    }

    const FilePath& DataFile::filePath() const noexcept {
        return _shared->path;
    }


    void DataFile::close() {
        for (auto& i : _keyStores) {
            i.second->close();
        }
        if (_shared->removeDataFile(this))
            LogTo(DBLog, "Closing DataFile");
    }


    void DataFile::reopen() {
        LogTo(DBLog, "Opening DataFile %s", filePath().path().c_str());
        _shared->addDataFile(this);
    }


    void DataFile::checkOpen() const {
        if (!isOpen())
            error::_throw(error::NotOpen);
    }


    void DataFile::rekey(EncryptionAlgorithm alg, slice newKey) {
        if (alg != kNoEncryption)
            error::_throw(error::UnsupportedEncryption);
    }


    void DataFile::forOtherDataFiles(function_ref<void(DataFile*)> fn) {
        _shared->forOpenDataFiles(this, fn);
    }


    Retained<RefCounted> DataFile::sharedObject(const string &key) {
        return _shared->sharedObject(key);
    }


    Retained<RefCounted>  DataFile::addSharedObject(const string &key, Retained<RefCounted> object) {
        return _shared->addSharedObject(key, object);
    }


#pragma mark - KEY-STORES:


    const string DataFile::kDefaultKeyStoreName{"default"};
    const string DataFile::kInfoKeyStoreName{"info"};


    KeyStore& DataFile::getKeyStore(const string &name) const {
        return getKeyStore(name, _options.keyStores);
    }

    KeyStore& DataFile::getKeyStore(const string &name, KeyStore::Capabilities options) const {
        checkOpen();
        auto i = _keyStores.find(name);
        if (i != _keyStores.end()) {
            KeyStore &store = *i->second;
            store.reopen();
            return store;
        } else {
            return const_cast<DataFile*>(this)->addKeyStore(name, options);
        }
    }

    KeyStore& DataFile::addKeyStore(const string &name, KeyStore::Capabilities options) {
        LogToAt(DBLog, Debug, "DataFile: open KVS '%s'", name.c_str());
        checkOpen();
        Assert(!(options.sequences && !_options.keyStores.sequences),
               "KeyStore can't have sequences if Database doesn't");
        KeyStore *store = newKeyStore(name, options);
        _keyStores[name] = unique_ptr<KeyStore>(store);
        return *store;
    }

    void DataFile::closeKeyStore(const string &name) {
        LogToAt(DBLog, Debug, "DataFile: close KVS '%s'", name.c_str());
        auto i = _keyStores.find(name);
        if (i != _keyStores.end()) {
            // Never remove a KeyStore from _keyStores: there may be objects pointing to it 
            i->second->close();
        }
    }

    KeyStore& DataFile::defaultKeyStore(KeyStore::Capabilities options) const {
        checkOpen();
        if (!_defaultKeyStore)
            const_cast<DataFile*>(this)->_defaultKeyStore = &getKeyStore(kDefaultKeyStoreName,
                                                                         options);
        return *_defaultKeyStore;
    }

    void DataFile::forOpenKeyStores(function_ref<void(KeyStore&)> fn) {
        for (auto& ks : _keyStores)
            fn(*ks.second);
    }


    SharedKeys* DataFile::documentKeys() const {
        auto keys = _documentKeys.get();
        if (!keys && _options.useDocumentKeys) {
            auto mutableThis = const_cast<DataFile*>(this);
            keys = new DocumentKeys(*mutableThis);
            mutableThis->_documentKeys.reset(keys);
        }
        return keys;
    }


#pragma mark - TRANSACTION:

    
    void DataFile::beginTransactionScope(Transaction* t) {
        Assert(!_inTransaction);
        checkOpen();
        _shared->setTransaction(t);
        _inTransaction = true;
    }

    void DataFile::transactionBegan(Transaction*) {
        if (_documentKeys)
            _documentKeys->transactionBegan();
    }

    void DataFile::transactionEnding(Transaction*, bool committing) {
        if (_documentKeys) {
            if (committing)
                _documentKeys->save();
            else
                _documentKeys->revert();
        }
    }
    
    void DataFile::endTransactionScope(Transaction* t) {
        _shared->unsetTransaction(t);
        _inTransaction = false;
        if (_documentKeys)
            _documentKeys->transactionEnded();
    }


    Transaction& DataFile::transaction() {
        Assert(_inTransaction);
        return *_shared->transaction();
    }


    void DataFile::withFileLock(function_ref<void(void)> fn) {
        if (_inTransaction) {
            fn();
        } else {
            Transaction t(this, false);
            fn();
        }
    }


    Transaction::Transaction(DataFile* db)
    :Transaction(db, true)
    { }

    Transaction::Transaction(DataFile* db, bool active)
    :_db(*db),
     _active(false)
    {
        _db.beginTransactionScope(this);
        if (active) {
            LogToAt(DBLog, Verbose, "DataFile: begin transaction");
            _db._beginTransaction(this);
            _active = true;
            _db.transactionBegan(this);
        }
    }


    void Transaction::commit() {
        Assert(_active, "Transaction is not active");
        _db.transactionEnding(this, true);
        _active = false;
        LogToAt(DBLog, Verbose, "DataFile: commit transaction");
        _db._endTransaction(this, true);
    }


    void Transaction::abort() {
        Assert(_active, "Transaction is not active");
        _db.transactionEnding(this, false);
        _active = false;
        LogTo(DBLog, "DataFile: abort transaction");
        _db._endTransaction(this, false);
    }


    Transaction::~Transaction() {
        if (_active) {
            LogTo(DBLog, "DataFile: Transaction exiting scope without explicit commit; aborting");
            abort();
        }
        _db.endTransactionScope(this);
    }


    ReadOnlyTransaction::ReadOnlyTransaction(DataFile *db) {
        db->beginReadOnlyTransaction();
        _db = db;
    }

    ReadOnlyTransaction::~ReadOnlyTransaction() {
        if (_db) {
            try {
                _db->endReadOnlyTransaction();
            } catch (...) {
                Warn("~ReadOnlyTransaction caught C++ exception in endReadOnlyTransaction");
            }
        }
    }

}
