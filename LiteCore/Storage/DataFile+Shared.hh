//
//  DataFile+Shared.hh
//  LiteCore
//
//  Created by Jens Alfke on 1/29/18.
//  Copyright 2018-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#pragma once
#include "Error.hh"
#include "Logging.hh"
#include "fleece/InstanceCounted.hh"
#include <mutex>               // std::mutex, std::unique_lock
#include <condition_variable>  // std::condition_variable
#include <unordered_map>
#include <algorithm>

namespace litecore {

    using namespace std;

    /** Shared state between all open DataFile instances on the same filesystem file.
        Manages a mutex that ensures that only one DataFile can open a transaction at once.
        This class is internal to DataFile. */
    class DataFile::Shared
        : public RefCounted
        , public fleece::InstanceCountedIn<DataFile::Shared>
        , Logging {
      public:
        static Retained<Shared> forPath(const FilePath &path, DataFile *dataFile) {
            string             pathStr = path.canonicalPath();
            unique_lock<mutex> lock(sFileMapMutex);
            Retained<Shared>   file = sFileMap[pathStr];
            if ( !file ) {
                file              = new Shared(pathStr);
                sFileMap[pathStr] = file;
                file->_logDebug("created for DataFile %p at %s", dataFile, pathStr.c_str());
            } else {
                file->_logDebug("adding DataFile %p", dataFile);
            }
            lock.unlock();

            if ( dataFile ) file->addDataFile(dataFile);
            return file;
        }

        static size_t openCountOnPath(const FilePath &path) {
            string pathStr = path.canonicalPath();

            unique_lock<mutex> lock(sFileMapMutex);
            Shared            *file = sFileMap[pathStr];
            return file ? file->openCount() : 0;
        }

        const string path;  // The filesystem path

        ExclusiveTransaction *transaction() { return _transaction; }

        void addDataFile(DataFile *dataFile) {
            unique_lock<mutex> lock(_mutex);
            mustNotBeCondemned();
            if ( find(_dataFiles.begin(), _dataFiles.end(), dataFile) == _dataFiles.end() )
                _dataFiles.push_back(dataFile);
        }

        bool removeDataFile(DataFile *dataFile) {
            unique_lock<mutex> lock(_mutex);
            logDebug("Remove DataFile %p", dataFile);
            auto pos = find(_dataFiles.begin(), _dataFiles.end(), dataFile);
            if ( pos == _dataFiles.end() ) return false;
            _dataFiles.erase(pos);
            if ( _dataFiles.empty() ) _sharedObjects.clear();
            return true;
        }

        void forOpenDataFiles(DataFile *except, function_ref<void(DataFile *)> fn) {
            unique_lock<mutex> lock(_mutex);
            for ( auto df : _dataFiles )
                if ( df != except && !df->isClosing() ) fn(df);
        }

        size_t openCount() {
            unique_lock<mutex> lock(_mutex);
            return _dataFiles.size();
        }

        // Marks the database file as about to be deleted, preventing any other thread from
        // opening (or deleting!) it.
        void condemn(bool condemn) {
            unique_lock<mutex> lock(_mutex);
            if ( condemn ) {
                mustNotBeCondemned();
                LogVerbose(DBLog, "Preparing to delete DataFile %s", path.c_str());
            }
            _condemned = condemn;
        }

        void setTransaction(ExclusiveTransaction *t) {
            Assert(t);
            unique_lock<mutex> lock(_transactionMutex);
            while ( _transaction != nullptr ) _transactionCond.wait(lock);
            _transaction = t;
        }

        void unsetTransaction(ExclusiveTransaction *t) {
            unique_lock<mutex> lock(_transactionMutex);
            Assert(t && _transaction == t);
            _transaction = nullptr;
            _transactionCond.notify_one();
        }

        Retained<RefCounted> sharedObject(const string &key) {
            lock_guard<mutex> lock(_mutex);
            auto              i = _sharedObjects.find(key);
            if ( i == _sharedObjects.end() ) return nullptr;
            return i->second;
        }

        Retained<RefCounted> addSharedObject(const string &key, RefCounted *object) {
            lock_guard<mutex> lock(_mutex);
            auto              e = _sharedObjects.emplace(key, object);
            return e.first->second;
        }


      protected:
        Shared(const string &p) : Logging(DBLog), path(p) { logDebug("instantiated on %s", p.c_str()); }

        ~Shared() {
            logDebug("destructing");
            unique_lock<mutex> lock(sFileMapMutex);
            sFileMap.erase(path);
        }

        void mustNotBeCondemned() {
            if ( _condemned ) error::_throw(error::Busy, "Database file is being deleted");
        }


      private:
        mutex                                       _transactionMutex;      // Mutex for transactions
        condition_variable                          _transactionCond;       // For waiting on the mutex
        ExclusiveTransaction                       *_transaction{nullptr};  // Currently active Transaction object
        vector<DataFile *>                          _dataFiles;             // Open DataFiles on this File
        unordered_map<string, Retained<RefCounted>> _sharedObjects;
        bool                                        _condemned{false};  // Prevents db from being opened or deleted
        mutex                                       _mutex;             // Mutex for non-transaction state

        static unordered_map<string, Shared *> sFileMap;
        static mutex                           sFileMapMutex;
    };

}  // namespace litecore
