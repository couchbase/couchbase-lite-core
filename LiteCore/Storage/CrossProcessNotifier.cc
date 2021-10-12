//
// CrossProcessNotifier.cc
//
// Copyright © 2021 Couchbase. All rights reserved.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "CrossProcessNotifier.hh"
#include "C4Error.h"
#include "Defer.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "ThreadUtil.hh"
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace litecore {
    using namespace std;

    static constexpr int kPermissions = 0700;

    struct CrossProcessNotifier::SharedData {
        uint32_t        lastPID;        // Process ID of last process that broadcast
        pthread_mutex_t mutex;
        pthread_cond_t  condition;      // Condition variable for coordinating
    };


    static int _check(const char *fn, int result) {
        if (result != 0)
            LogToAt(DBLog, Warning, "error %d from %s", result, fn);
        return result;
    }

    int CrossProcessNotifier::_check(const char *fn, int result) const {
        if (result != 0)
            warn("error %d from %s", result, fn);
        return result;
    }

    #define check(EXPR) _check(#EXPR, EXPR)


    class MutexLocker {
    public:
        MutexLocker(pthread_mutex_t &mutex) :_mutex(&mutex) {check(::pthread_mutex_lock(_mutex));}
        ~MutexLocker()                      {check(::pthread_mutex_unlock(_mutex));}
    private:
        pthread_mutex_t* _mutex;
    };

    #define LOCK(MUTEX)     MutexLocker _lock(MUTEX)


    CrossProcessNotifier::CrossProcessNotifier()
    :Logging(DBLog)
    { }


    CrossProcessNotifier::~CrossProcessNotifier() {
        teardown();
        logVerbose("Deleted");
    }


    string CrossProcessNotifier::loggingIdentifier() const {
        return _path;
    }



    bool CrossProcessNotifier::start(const FilePath &databaseDir,
                                     Callback callback,
                                     C4Error *outError)
    {
        // Open the shared-memory file:
        int err;
        FilePath memFile = databaseDir[kSharedMemFilename];
        _path = memFile.path();
        int fd = ::open(memFile.path().c_str(), O_CREAT | O_RDWR, kPermissions);
        if (fd < 0) {
            err = errno;
            _check("open()", err);
            C4Error::set(outError, POSIXDomain, err, "Couldn't open shared-memory file");
            return false;
        }
        // Ensure the file is non-empty, without deleting any existing contents:
        ::ftruncate(fd, 4096);

        // Map it read-write:
        void *mapped = ::mmap(nullptr,
                              sizeof(SharedData),
                              PROT_READ | PROT_WRITE,
                              MAP_FILE | MAP_SHARED,
                              fd,
                              0);
        err = errno;
        ::close(fd);
        if (mapped == MAP_FAILED) {
            _check("mmap()", err);
            C4Error::set(outError, POSIXDomain, errno,
                         "Couldn't memory-map shared-memory file");
            return false;
        }
        _sharedData = (SharedData*)mapped;

        // Initialize the mutex and condition variable in the shared memory:
        pthread_mutexattr_t mattr;
        ::pthread_mutexattr_init(&mattr);
        err = check(::pthread_mutexattr_setpshared(&mattr, true));
        ::pthread_mutex_init(&_sharedData->mutex, &mattr);
        ::pthread_mutexattr_destroy(&mattr);
        if (err) {
            teardown();
            C4Error::set(outError, POSIXDomain, err, "Unable to create a shared mutex");
            return false;
        }

        // Create a condition variable:
        pthread_condattr_t cattr;
        ::pthread_condattr_init(&cattr);
        check(::pthread_condattr_setpshared(&cattr, true));   // make it work between processes
        err = check(::pthread_cond_init(&_sharedData->condition, &cattr));
        ::pthread_condattr_destroy(&cattr);
        if (err) {
            teardown();
            C4Error::set(outError, POSIXDomain, err,"Unable to create a shared condition lock");
            return false;
        }

        logInfo("Initialized");
        _myPID = getpid();
        _callback = callback;

        // Now start the observer thread:
        _running = true;
        thread([this] { observe(this); }).detach();

        return true;
    }


    void CrossProcessNotifier::teardown() {
        if (_sharedData) {
            ::munmap(_sharedData, sizeof(SharedData));
            _sharedData = nullptr;
        }
    }


    void CrossProcessNotifier::stop() {
        if (_running) {
            logVerbose("Stopping...");
            // Clear the `_running` flag and trigger a notification to wake up my thread.
            // (Unfortunately this wakes up all other observing processes; I don't know how to
            // get around that. The others will ignore it.)
            LOCK(_sharedData->mutex);
            _running = false;
            _callback = nullptr;
            _sharedData->lastPID = -1;
            check(::pthread_cond_broadcast(&_sharedData->condition));
        }
    }


    void CrossProcessNotifier::notify() const {
        if (!_sharedData)
            return;
        logInfo("Posting notification (from PID %d)", _myPID);
        LOCK(_sharedData->mutex);
        _sharedData->lastPID = _myPID;
        check(::pthread_cond_broadcast(&_sharedData->condition));
    }


    void CrossProcessNotifier::observe(Retained<CrossProcessNotifier> selfRetain) {
        SetThreadName("CBL Cross-Process Notifier");

        bool running;
        do {
            logVerbose("Waiting...");
            int notifierPID;
            Callback callback;
            {
                LOCK(_sharedData->mutex);
                if (check(::pthread_cond_wait(&_sharedData->condition, &_sharedData->mutex)) != 0)
                    break;
                notifierPID = _sharedData->lastPID;
                running = _running;
                callback = _callback;
            }

            if (notifierPID != _myPID && notifierPID != -1 && callback) {
                logVerbose("Notified by pid %d! Invoking callback()...",
                      _sharedData->lastPID);
                callback();
            }
        } while (running);

        logVerbose("Thread stopping");
        teardown();
    }

}
