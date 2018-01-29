//
//  DataFile+Shared.hh
//  LiteCore
//
//  Created by Jens Alfke on 1/29/18.
//  Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once
#include "Error.hh"
#include "c4Private.h"        // C4InstanceCounted
#include <mutex>              // std::mutex, std::unique_lock
#include <condition_variable> // std::condition_variable
#include <unordered_map>


namespace litecore {

    using namespace std;


    /** Shared state between all open DataFile instances on the same filesystem file.
        Manages a mutex that ensures that only one DataFile can open a transaction at once.
        This class is internal to DataFile. */
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


        Retained<RefCounted> addSharedObject(const string &key, Retained<RefCounted> object) {
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

}

