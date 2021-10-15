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
#include "CrossProcessNotifier.hh"
#include "C4Error.h"
#include "Error.hh"
#include "Logging.hh"
#include "InstanceCounted.hh"
#include <condition_variable> // std::condition_variable
#include <algorithm>
#include <mutex>              // std::mutex, std::unique_lock
#include <optional>
#include <unordered_map>

namespace litecore {

    using namespace std;


    /** Shared state between all open DataFile instances on the same filesystem file.
        Manages a mutex that ensures that only one DataFile can open a transaction at once.
        This class is internal to DataFile. */
    class DataFile::Shared : public RefCounted,
                             public fleece::InstanceCountedIn<DataFile::Shared>,
                             Logging
    {
    public:

        /// Returns the `Shared` instance for the given filesystem path, creating it if needed.
        /// @param path  The filesystem path.
        /// @param dataFile  DataFile to register with the instance, or null.
        /// @return  The `Shared` instance.
        static Retained<Shared> forPath(const FilePath &path, DataFile *dataFile) {
            string pathStr = path.canonicalPath();
            unique_lock<mutex> lock(sGlobalFileMapMutex);
            Retained<Shared> shared = sGlobalFileMap[pathStr];
            if (!shared) {
                shared = new Shared(pathStr);
                sGlobalFileMap[pathStr] = shared;
                shared->_logDebug("created for DataFile %p at %s", dataFile, pathStr.c_str());
            } else {
                if (dataFile)
                    shared->_logDebug("adding DataFile %p", dataFile);
            }
            lock.unlock();

            if (dataFile)
                shared->addDataFile(dataFile);
            return shared;
        }


        const string& path() const {
            return _path;
        }


        ExclusiveTransaction* transaction() {
            return _transaction;
        }


        void addDataFile(DataFile *dataFile) {
            unique_lock<mutex> lock(_mutex);
            mustNotBeCondemned();
            createCrossProcessNotifier();
            if (find(_dataFiles.begin(), _dataFiles.end(), dataFile) == _dataFiles.end())
                _dataFiles.push_back(dataFile);
        }


        bool removeDataFile(DataFile *dataFile) {
            unique_lock<mutex> lock(_mutex);
            logDebug("Remove DataFile %p", dataFile);
            auto pos = find(_dataFiles.begin(), _dataFiles.end(), dataFile);
            if (pos == _dataFiles.end())
                return false;
            _dataFiles.erase(pos);
            if (_dataFiles.empty())
                _sharedObjects.clear();
            return true;
        }


        void forOpenDataFiles(DataFile *except, function_ref<void(DataFile*)> fn) {
            unique_lock<mutex> lock(_mutex);
            for (auto df : _dataFiles)
                if (df != except && !df->isClosing())
                    fn(df);
        }


        size_t openCount() {
            unique_lock<mutex> lock(_mutex);
            return _dataFiles.size();
        }


        // Marks the database file as about to be deleted, preventing any other thread from
        // opening (or deleting!) it.
        void condemn(bool condemn) {
            unique_lock<mutex> lock(_mutex);
            if (condemn) {
                mustNotBeCondemned();
                LogVerbose(DBLog, "Preparing to delete DataFile %s", _path.c_str());
            }
            _condemned = condemn;
        }


        void setTransaction(ExclusiveTransaction* t) {
            Assert(t);
            unique_lock<mutex> lock(_transactionMutex);
            while (_transaction != nullptr)
                _transactionCond.wait(lock);
            _transaction = t;
        }


        void unsetTransaction(ExclusiveTransaction* t) {
            unique_lock<mutex> lock(_transactionMutex);
            Assert(t && _transaction == t);
            bool committed = t->committed();
            _transaction = nullptr;
            _transactionCond.notify_one();

            if (committed && _xpNotifier) {
                logInfo("Posting cross-process transaction notification");
                _xpNotifier->notify();
            }
        }


        Retained<RefCounted> sharedObject(const string &key) {
            lock_guard<mutex> lock(_mutex);
            auto i = _sharedObjects.find(key);
            if (i == _sharedObjects.end())
                return nullptr;
            return i->second;
        }


        Retained<RefCounted> addSharedObject(const string &key, RefCounted *object) {
            lock_guard<mutex> lock(_mutex);
            auto e = _sharedObjects.emplace(key, object);
            return e.first->second;
        }


        // Deletes the file used by the CrossProcessNotifier.
        bool deleteNotifierFile() {
            if (_xpNotifier) {
                _xpNotifier->stop();
                _xpNotifier = nullptr;
            }
            return notifierPath().del();
        }


    private:
        using GlobalFileMap = unordered_map<string, Shared*>;
        using SharedObjectMap = unordered_map<string, Retained<RefCounted>>;

        static constexpr const char* kSharedMemFilename = "cblite_mem";

        Shared(const string &path)
        :Logging(DBLog)
        ,_path(path)
        {
            logDebug("instantiated on %s", path.c_str());
        }


        ~Shared() {
            logDebug("destructing");
            if (_xpNotifier)
                _xpNotifier->stop();
            unique_lock<mutex> lock(sGlobalFileMapMutex);
            sGlobalFileMap.erase(_path);
        }


        string loggingIdentifier() const override {
            return _path;
        }

        
        void mustNotBeCondemned() {
            if (_condemned)
                error::_throw(error::Busy, "Database file is being deleted");
        }


        FilePath notifierPath() const {
            return FilePath(_path).withExtension("cblite_shmem");
        }


        void createCrossProcessNotifier() {
            if (_xpNotifier)
                return;
            _xpNotifier = new CrossProcessNotifier();

            auto observer = [&] {
                // This is the callback that runs when another process updates the database:
                logInfo("Cross-process notification received!!!");
                unique_lock<mutex> lock(_mutex);
                for (DataFile *df : _dataFiles)
                    df->delegate()->externalTransactionCommitted(nullptr);
            };

            C4Error error;
            if (!_xpNotifier->start(notifierPath(), observer, &error))
                warn("Couldn't start cross-process notifier: %s", error.description().c_str());
        }


        const string            _path;                   // The filesystem path
        vector<DataFile*>       _dataFiles;              // The open DataFiles on this file
        bool                    _condemned {false};      // Prevents db from being opened or deleted
        SharedObjectMap         _sharedObjects;          // Named object store for clients to use
        Retained<CrossProcessNotifier> _xpNotifier;      // Cross-process change notifier/observer
        mutex                   _mutex;                  // Mutex for the above state

        ExclusiveTransaction*   _transaction {nullptr};  // Currently active Transaction object
        mutex                   _transactionMutex;       // Mutex for transactions
        condition_variable      _transactionCond;        // For waiting on the transaction mutex


        // static/global data:
        static inline GlobalFileMap    sGlobalFileMap;
        static inline mutex            sGlobalFileMapMutex;
    };

}

