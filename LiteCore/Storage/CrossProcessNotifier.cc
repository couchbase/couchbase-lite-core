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
#include "CrossProcessNotifierData.hh"
#include "C4Error.h"
#include "ThreadUtil.hh"
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace litecore {
    using namespace std;

#pragma mark - UTILITIES:


    static int _check(const char *fn, int result) {
        if (result != 0)
            LogToAt(DBLog, Error, "%s (%d) from %s", strerror(result), result, fn);
        return result;
    }

    #define check(EXPR) _check(#EXPR, EXPR)


#pragma mark - SHARED DATA:


    // File permissions for the shared-memory file. Allows read+write, for owner only.
    static constexpr int kFilePermissions = 0600;



    #define LOCK(DATA)     CrossProcessNotifierData::Lock _lock(DATA)


#pragma mark - CROSS-PROCESS NOTIFIER:


    CrossProcessNotifier::~CrossProcessNotifier() {
        teardown();
        logVerbose("Deleted");
    }


    bool CrossProcessNotifier::start(const string &path, Callback callback, C4Error *outError) {
        // Open/create the shared-memory file:
        _path = std::move(path);
        int err;
        int fd = ::open(_path.c_str(), O_CREAT | O_RDWR, kFilePermissions);
        if (fd < 0) {
            err = errno;
            logError("%s (%d) opening shared-memory file", strerror(err), err);
            C4Error::set(outError, POSIXDomain, err, "Couldn't open shared-memory file %s",
                         _path.c_str());
            return false;
        }
        // Ensure the file is non-empty, without deleting any existing contents. Leave enough room
        // for any potential future expansion of the data:
        ::ftruncate(fd, 4096);

        // Memory-map it, read-write & shared. After this the file descriptor can be closed:
        void *mapped = ::mmap(nullptr,
                              sizeof(CrossProcessNotifierData),
                              PROT_READ | PROT_WRITE,
                              MAP_FILE | MAP_SHARED,
                              fd,
                              0);
        err = errno;
        ::close(fd);
        if (mapped == MAP_FAILED) {
            logError("%s (%d) memory-mapping file", strerror(err), err);
            C4Error::set(outError, POSIXDomain, err,
                         "Couldn't memory-map shared-memory file %s", _path.c_str());
            return false;
        }
        _sharedData = (CrossProcessNotifierData*)mapped;

        // Check the file contents, and initialize if necessary:
        if (!_sharedData->valid()) {
            if (_sharedData->uninitialized())
                logInfo("Initializing shared memory notifier file");
            else
                warn("Shared memory is invalid; re-initializing it");
            if (auto [error, fn] = _sharedData->initialize(); error != 0) {
                warn("Couldn't initialize notifier in file %s; %s failed",
                     _path.c_str(), fn);
                C4Error::set(outError, POSIXDomain, err,
                             "Couldn't initialize notifier in file %s; %s failed",
                             _path.c_str(), fn);
                return false;
            }
        }

        // Now start the observer thread:
        logInfo("Initialized");
        _myPID = getpid();
        _callback = callback;
        _running = true;
        thread([this] { observerThread(this); }).detach();

        return true;
    }


    void CrossProcessNotifier::teardown() {
        _running = false;
        if (_sharedData) {
            ::munmap(_sharedData, sizeof(CrossProcessNotifierData));
            _sharedData = nullptr;
        }
    }


    void CrossProcessNotifier::stop() {
        if (_running) {
            logVerbose("Stopping...");
            // Clear the `_running` flag and trigger a notification to wake up my thread,
            // which will detect the cleared flag and stop.
            // (Unfortunately this wakes up all other observing processes; I don't know how to
            // get around that. The others will ignore it.)
            LOCK(_sharedData);
            _running = false;
            _callback = nullptr;
            _sharedData->broadcast(-1);
        }
    }


    void CrossProcessNotifier::notify() const {
        if (_sharedData)
            _sharedData->broadcast(_myPID);
    }


    void CrossProcessNotifier::observerThread(Retained<CrossProcessNotifier> selfRetain) {
        SetThreadName("CBL Cross-Process Notifier");

        for (bool running = _running; running; ) {
            logVerbose("Waiting...");
            int notifyingPID;
            Callback callback;
            {
                LOCK(_sharedData);
                if (check(_sharedData->wait(&notifyingPID)) != 0)
                    break;
                running = _running;
                callback = _callback;
            }

            if (notifyingPID != _myPID && notifyingPID != -1 && callback) {
                logVerbose("Notified by pid %d! Invoking callback()...", notifyingPID);
                try {
                    callback();
                } catch (...) {
                    C4Error::warnCurrentException("CrossProcessNotifier::observerThread");
                }
            }
        }

        logVerbose("Thread stopping");
        teardown();
    }

}
