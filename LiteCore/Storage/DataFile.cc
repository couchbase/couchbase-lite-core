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
#include "Document.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "LogInternal.hh"
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


    DataFile::Factory* DataFile::factoryNamed(std::string name) {
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

    class DataFile::File {
    public:
        static File* forPath(const FilePath &path);
        File(const FilePath &path)      :_path(path) { }

        const FilePath _path;
        mutex _transactionMutex;
        condition_variable _transactionCond;
        Transaction* _transaction {NULL};
        atomic<bool> _isCompacting {false};

        static unordered_map<string, File*> sFileMap;
        static mutex sMutex;
    };

    unordered_map<string, DataFile::File*> DataFile::File::sFileMap;
    mutex DataFile::File::sMutex;

    DataFile::File* DataFile::File::forPath(const FilePath &path) {
        unique_lock<mutex> lock(sMutex);
        auto pathStr = path.path();
        File* file = sFileMap[pathStr];
        if (!file) {
            file = new File(path);
            sFileMap[pathStr] = file;
        }
        return file;
    }


#pragma mark - DATABASE:


    const DataFile::Options DataFile::Options::defaults = DataFile::Options {
        {true, true, false},
        true, true};


    DataFile::DataFile(const FilePath &path, const DataFile::Options *options)
    :_file(File::forPath(path)),
     _options(options ? *options : Options::defaults)
    { }

    DataFile::~DataFile() {
        Debug("DataFile: deleting (~DataFile)");
        CBFAssert(!_inTransaction);
    }

    const FilePath& DataFile::filePath() const noexcept {
        return _file->_path;
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
        Debug("DataFile: open KVS '%s'", name.c_str());
        checkOpen();
        CBFAssert(!(options.sequences && !_options.keyStores.sequences));
        CBFAssert(!(options.softDeletes && !_options.keyStores.softDeletes));
        KeyStore *store = newKeyStore(name, options);
        _keyStores[name] = unique_ptr<KeyStore>(store);
        return *store;
    }

    void DataFile::closeKeyStore(const string &name) {
        Debug("DataFile: close KVS '%s'", name.c_str());
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


#pragma mark PURGE/DELETION COUNT:


    const string DataFile::kInfoKeyStoreName = "info";

    static const char* const kDeletionCountKey = "deletionCount";
    static const char* const kPurgeCountKey = "purgeCount";

    void DataFile::incrementDeletionCount(Transaction &t) {
        KeyStore &infoStore = getKeyStore(kInfoKeyStoreName);
        Document doc = infoStore.get(slice(kDeletionCountKey));
        uint64_t purgeCount = doc.bodyAsUInt() + 1;
        uint64_t newBody = _endian_encode(purgeCount);
        doc.setBody(slice(&newBody, sizeof(newBody)));
        infoStore.write(doc, t);
    }

    uint64_t DataFile::purgeCount() const {
        return getKeyStore(kInfoKeyStoreName).get(slice(kPurgeCountKey)).bodyAsUInt();
    }

    void DataFile::updatePurgeCount(Transaction &t) {
        KeyStore& infoStore = getKeyStore(kInfoKeyStoreName);
        Document purgeCount = infoStore.get(slice(kDeletionCountKey));
        if (purgeCount.exists())
            infoStore.set(slice(kPurgeCountKey), purgeCount.body(), t);
    }


#pragma mark - TRANSACTION:

    void DataFile::beginTransactionScope(Transaction* t) {
        CBFAssert(!_inTransaction);
        checkOpen();
        unique_lock<mutex> lock(_file->_transactionMutex);
        while (_file->_transaction != NULL)
            _file->_transactionCond.wait(lock);
        _file->_transaction = t;
        _inTransaction = true;
    }

    void DataFile::endTransactionScope(Transaction* t) {
        unique_lock<mutex> lock(_file->_transactionMutex);
        CBFAssert(_file->_transaction == t);
        _file->_transaction = NULL;
        _file->_transactionCond.notify_one();
        _inTransaction = false;
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
            Log("DataFile: beginTransaction");
            _db._beginTransaction(this);
            _active = true;
        }
    }


    void Transaction::commit() {
        CBFAssert(_active);
        _active = false;
        Log("DataFile: commit transaction");
        _db._endTransaction(this, true);
    }


    void Transaction::abort() {
        CBFAssert(_active);
        _active = false;
        Log("DataFile: abort transaction");
        _db._endTransaction(this, false);
    }


    Transaction::~Transaction() {
        if (_active) {
            Log("DataFile: Transaction exiting scope without explicit commit; aborting");
            _db._endTransaction(this, false);
        }
        _db.endTransactionScope(this);
    }
    

#pragma mark - COMPACTION:


    static atomic<uint32_t> sCompactCount;


    void DataFile::beganCompacting() {
        ++sCompactCount;
        _file->_isCompacting = true;
        if (_onCompactCallback) _onCompactCallback(true);
    }
    void DataFile::finishedCompacting() {
        --sCompactCount;
        _file->_isCompacting = false;
        if (_onCompactCallback) _onCompactCallback(false);
    }

    bool DataFile::isCompacting() const noexcept {
        return _file->_isCompacting;
    }

    bool DataFile::isAnyCompacting() noexcept {
        return sCompactCount > 0;
    }

}
