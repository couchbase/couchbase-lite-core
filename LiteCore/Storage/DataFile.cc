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
#include "forestdb_endian.h"
#include <errno.h>
#include <mutex>              // std::mutex, std::unique_lock
#include <condition_variable> // std::condition_variable
#include <unordered_map>
#include <dirent.h>
#ifdef _MSC_VER
#include <asprintf.h>
#else
#include <unistd.h>
#endif

#include "ForestDataFile.hh"
#include "SQLiteDataFile.hh"

using namespace std;


namespace litecore {

    LogDomain DBLog("DB");


#pragma mark - FACTORY:

    bool DataFile::Factory::deleteFile(const FilePath &path, const Options*) {
        return path.delWithAllExtensions();
    }

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
        return {&SQLiteDataFile::factory(), &ForestDataFile::factory()};
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



#pragma mark - FILE:


    /** Shared state between all open DataFile instances on the same filesystem file.
        Manages a mutex that ensures that only one DataFile can open a transaction at once. */
    class DataFile::File {
    public:
        static File* forPath(const FilePath &path);
        File(const FilePath &p)      :path(p) { }

        void setTransaction(Transaction*);
        void unsetTransaction(Transaction*);
        Transaction* transaction()                      {return _transaction;}

        const FilePath path;                            // The filesystem path
        atomic<bool> isCompacting {false};              // Is the database compacting?

    private:
        mutex _transactionMutex;                        // Mutex for transactions
        condition_variable _transactionCond;            // For waiting on the mutex
        Transaction* _transaction {nullptr};            // Currently active Transaction object

        static unordered_map<string, File*> sFileMap;
        static mutex sFileMapMutex;
    };


    unordered_map<string, DataFile::File*> DataFile::File::sFileMap;
    mutex DataFile::File::sFileMapMutex;


    DataFile::File* DataFile::File::forPath(const FilePath &path) {
        unique_lock<mutex> lock(sFileMapMutex);
        auto pathStr = path.path();
        File* file = sFileMap[pathStr];
        if (!file) {
            file = new File(path);
            sFileMap[pathStr] = file;
        }
        return file;
    }


    void DataFile::File::setTransaction(Transaction* t) {
        Assert(t);
        unique_lock<mutex> lock(_transactionMutex);
        while (_transaction != nullptr)
            _transactionCond.wait(lock);
        _transaction = t;
    }


    void DataFile::File::unsetTransaction(Transaction* t) {
        unique_lock<mutex> lock(_transactionMutex);
        Assert(t && _transaction == t);
        _transaction = nullptr;
        _transactionCond.notify_one();
    }


#pragma mark - DATAFILE:


    const DataFile::Options DataFile::Options::defaults = DataFile::Options {
        {true, true, false},
        true, true};


    DataFile::DataFile(const FilePath &path, const DataFile::Options *options)
    :_file(File::forPath(path)),
     _options(options ? *options : Options::defaults)
    { }

    DataFile::~DataFile() {
        LogToAt(DBLog, Debug, "DataFile: deleting (~DataFile)");
        Assert(!_inTransaction);
    }

    const FilePath& DataFile::filePath() const noexcept {
        return _file->path;
    }


    void DataFile::close() {
        for (auto& i : _keyStores) {
            i.second->close();
        }
    }


    void DataFile::checkOpen() const {
        if (!isOpen())
            error::_throw(error::NotOpen);
    }


    void DataFile::rekey(EncryptionAlgorithm alg, slice newKey) {
        if (alg != kNoEncryption)
            error::_throw(error::UnsupportedEncryption);
    }


#pragma mark KEY-STORES:


    const string DataFile::kDefaultKeyStoreName{"default"};


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
        Assert(!(options.softDeletes && !_options.keyStores.softDeletes),
               "KeyStore can't have softDeletes if Database doesn't");
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

    void DataFile::forOpenKeyStores(std::function<void(KeyStore&)> fn) {
        for (auto& ks : _keyStores)
            fn(*ks.second);
    }


    void DataFile::useDocumentKeys() {
        if (!_documentKeys.get())
            _documentKeys.reset(new DocumentKeys(*this));
    }



#pragma mark PURGE/DELETION COUNT:


    const string DataFile::kInfoKeyStoreName = "info";

    static const char* const kDeletionCountKey = "deletionCount";
    static const char* const kPurgeCountKey = "purgeCount";

    void DataFile::incrementDeletionCount(Transaction &t) {
        KeyStore &infoStore = getKeyStore(kInfoKeyStoreName);
        Record rec = infoStore.get(slice(kDeletionCountKey));
        uint64_t purgeCount = rec.bodyAsUInt() + 1;
        uint64_t newBody = _endian_encode(purgeCount);
        rec.setBody(slice(&newBody, sizeof(newBody)));
        infoStore.write(rec, t);
    }

    uint64_t DataFile::purgeCount() const {
        return getKeyStore(kInfoKeyStoreName).get(slice(kPurgeCountKey)).bodyAsUInt();
    }

    void DataFile::updatePurgeCount(Transaction &t) {
        KeyStore& infoStore = getKeyStore(kInfoKeyStoreName);
        Record purgeCount = infoStore.get(slice(kDeletionCountKey));
        if (purgeCount.exists())
            infoStore.set(slice(kPurgeCountKey), purgeCount.body(), t);
    }


#pragma mark - TRANSACTION:

    void DataFile::beginTransactionScope(Transaction* t) {
        Assert(!_inTransaction);
        checkOpen();
        _file->setTransaction(t);
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
        _file->unsetTransaction(t);
        _inTransaction = false;
        if (_documentKeys)
            _documentKeys->transactionEnded();
    }


    Transaction& DataFile::transaction() {
        Assert(_inTransaction);
        return *_file->transaction();
    }


    void DataFile::withFileLock(function<void(void)> fn) {
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
            LogTo(DBLog, "DataFile: beginTransaction");
            _db._beginTransaction(this);
            _active = true;
            _db.transactionBegan(this);
        }
    }


    void Transaction::commit() {
        Assert(_active, "Transaction is not active");
        _db.transactionEnding(this, true);
        _active = false;
        LogTo(DBLog, "DataFile: commit transaction");
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
    

#pragma mark - COMPACTION:


    static atomic<uint32_t> sCompactCount;


    void DataFile::beganCompacting() {
        ++sCompactCount;
        _file->isCompacting = true;
        if (_onCompactCallback) _onCompactCallback(true);
    }
    void DataFile::finishedCompacting() {
        --sCompactCount;
        _file->isCompacting = false;
        if (_onCompactCallback) _onCompactCallback(false);
    }

    bool DataFile::isCompacting() const noexcept {
        return _file->isCompacting;
    }

    bool DataFile::isAnyCompacting() noexcept {
        return sCompactCount > 0;
    }

}
