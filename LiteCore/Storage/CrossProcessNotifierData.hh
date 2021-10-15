//
// CrossProcessNotifier_Data.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include <utility>
#include <pthread.h>
#include <stdlib.h>

namespace litecore {

    class CrossProcessNotifierData {
    public:
        bool uninitialized() const  { return _magic == 0; }

        bool valid() {
            if (_magic != kMagic)
                return false;
            else if (::pthread_mutex_lock(&_mutex) != 0)
                return false;
            else {
                ::pthread_mutex_unlock(&_mutex);
                return true;
            }
        }

        std::pair<int,const char*> initialize() {
            _magic = kMagic;

            // Create a mutex with shared-memory support:
            int err;
            pthread_mutexattr_t mattr;
            ::pthread_mutexattr_init(&mattr);
            if (err = ::pthread_mutexattr_setpshared(&mattr, true); err)
                return {err, "pthread_mtexattr_setpshared"};
            ::pthread_mutex_init(&_mutex, &mattr);
            if (err = ::pthread_mutexattr_setpshared(&mattr, true); err)
                return {err, "pthread_mutex_init"};
            ::pthread_mutexattr_destroy(&mattr);

            // Create a condition variable with shared-memory support:
            pthread_condattr_t cattr;
            ::pthread_condattr_init(&cattr);
            if (err = ::pthread_condattr_setpshared(&cattr, true); err) {
                ::pthread_mutex_destroy(&_mutex);
                return {err, "pthread_condattr_setpshared"};
            }
            if (err = ::pthread_cond_init(&_condition, &cattr); err) {
                ::pthread_mutex_destroy(&_mutex);
                return {err, "pthread_cond_init"};
            }
            ::pthread_condattr_destroy(&cattr);

            _lastPID = -1;
            return {0, nullptr};
        }

        // must be called while locked!
        int broadcast(int pid) {
            _lastPID = pid;
            return ::pthread_cond_broadcast(&_condition);
        }

        // must be called while locked!
        int wait(int *outPID) {
            int err = ::pthread_cond_wait(&_condition, &_mutex);
            *outPID = _lastPID;
            return err;
        }


        class Lock {
        public:
            Lock(CrossProcessNotifierData *data) :_data(data) {::pthread_mutex_lock(&_data->_mutex);}
            ~Lock()                             {::pthread_mutex_unlock(&_data->_mutex);}
        private:
            CrossProcessNotifierData* _data;
        };

    private:
        static constexpr uint32_t kMagic = htonl(0x43424C54);

        uint32_t        _magic;          // Identifies file format
        pthread_mutex_t _mutex;          // Controls access to the rest of the data
        pthread_cond_t  _condition;      // For notifying / listening
        int32_t         _lastPID;        // Process ID of last process that broadcast
    };

}
