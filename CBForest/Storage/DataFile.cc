//
//  DataFile.cc
//  CBForest
//
//  Created by Jens Alfke on 5/12/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
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
#include "LogInternal.hh"
#include "forestdb_endian.h"
#include <errno.h>
#include <mutex>              // std::mutex, std::unique_lock
#include <condition_variable> // std::condition_variable
#include <unordered_map>
#include <dirent.h>
#include <unistd.h>
#ifdef _MSC_VER
#include <asprintf.h>
#endif

using namespace std;


namespace cbforest {

#pragma mark - FILE:

    class DataFile::File {
    public:
        static File* forPath(const string &path);
        File(const string &path)      :_path(path) { }
        const string _path;
        mutex _transactionMutex;
        condition_variable _transactionCond;
        Transaction* _transaction {NULL};
        atomic<bool> _isCompacting {false};

        static unordered_map<string, File*> sFileMap;
        static mutex sMutex;
    };

    unordered_map<string, DataFile::File*> DataFile::File::sFileMap;
    mutex DataFile::File::sMutex;

    DataFile::File* DataFile::File::forPath(const string &path) {
        unique_lock<mutex> lock(sMutex);
        File* file = sFileMap[path];
        if (!file) {
            file = new File(path);
            sFileMap[path] = file;
        }
        return file;
    }


#pragma mark - DATABASE:


    const DataFile::Options DataFile::Options::defaults = DataFile::Options {
        {true, true, false},
        true, true};


    DataFile::DataFile(const string &path, const DataFile::Options *options)
    :_file(File::forPath(path)),
     _options(options ? *options : Options::defaults)
    { }

    DataFile::~DataFile() {
        Debug("DataFile: deleting (~DataFile)");
        CBFAssert(!_inTransaction);
    }

    const string& DataFile::filename() const {
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


    void DataFile::deleteDataFile(const string &path) {
#ifdef _MSC_VER
        static const char kSeparatorChar = '\\';
        static const char* kCurrentDir = ".\\";
#else
        static const char kSeparatorChar = '/';
        static const char* kCurrentDir = "./";
#endif
        // Split path into directory name and base filename:
        string dirname, basename;
        auto slash = path.rfind(kSeparatorChar);           //FIX: OS-dependent
        if (slash == string::npos) {
            dirname = string(kCurrentDir);
            basename = path;
        } else {
            dirname = path.substr(0, slash+1);
            basename = path.substr(slash+1);
        }

        // Scan the directory:
        auto dir = opendir(dirname.c_str());
        if (!dir)
            error::_throw(error::POSIX, errno);
        struct dirent entry, *result;
        int err;
        while (1) {
            err = readdir_r(dir, &entry, &result);
            if (err || !result)
                break;
            if (result->d_type == DT_REG) {
                string name(result->d_name, result->d_namlen);
                if (name.find(basename) == 0) {
                    // Delete a file whose name starts with the basename:
                    if (::unlink((dirname + name).c_str()) != 0)
                        error::_throw(error::POSIX, errno);
                }
            }
        }
        closedir(dir);
        if (err)
            error::_throw(error::POSIX, err);
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


    static const char* const kInfoStoreName = "info";
    static const char* const kDeletionCountKey = "deletionCount";
    static const char* const kPurgeCountKey = "purgeCount";

    static uint64_t readCount(const Document &doc) {
        uint64_t count;
        if (doc.body().size < sizeof(count))
            return 0;
        memcpy(&count, doc.body().buf, sizeof(count));
        return _endian_decode(count);
    }

    void DataFile::incrementDeletionCount(Transaction &t) {
        KeyStore &infoStore = getKeyStore(kInfoStoreName);
        Document doc = infoStore.get(slice(kDeletionCountKey));
        uint64_t purgeCount = readCount(doc) + 1;
        uint64_t newBody = _endian_encode(purgeCount);
        doc.setBody(slice(&newBody, sizeof(newBody)));
        infoStore.write(doc, t);
    }

    uint64_t DataFile::purgeCount() const {
        KeyStore &infoStore = getKeyStore(kInfoStoreName);
        return readCount( infoStore.get(slice(kPurgeCountKey)) );
    }

    void DataFile::updatePurgeCount(Transaction &t) {
        KeyStore& infoStore = getKeyStore(kInfoStoreName);
        Document purgeCount = infoStore.get(slice(kDeletionCountKey));
        if (purgeCount.exists())
            infoStore.set(slice(kPurgeCountKey), purgeCount.body(), t);
    }


#pragma mark - TRANSACTION:

    void DataFile::beginTransaction(Transaction* t) {
        CBFAssert(!_inTransaction);
        checkOpen();
        unique_lock<mutex> lock(_file->_transactionMutex);
        while (_file->_transaction != NULL)
            _file->_transactionCond.wait(lock);

        if (t->state() >= Transaction::kCommit) {
            Log("DataFile: beginTransaction");
            _beginTransaction(t);
        }
        _file->_transaction = t;
        _inTransaction = true;
    }

    void DataFile::endTransaction(Transaction* t) {
        if (t->state() != Transaction::kNoOp)
            _endTransaction(t);

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
    :_db(*db),
     _state(kCommit)
    {
        _db.beginTransaction(this);
    }

    Transaction::Transaction(DataFile* db, bool begin)
    :_db(*db),
     _state(begin ? kCommit : kNoOp)
    {
        _db.beginTransaction(this);
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

    bool DataFile::isCompacting() const {
        return _file->_isCompacting;
    }

    bool DataFile::isAnyCompacting() {
        return sCompactCount > 0;
    }

}
