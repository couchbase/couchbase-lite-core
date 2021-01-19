//
// Timer.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <condition_variable>

namespace litecore { namespace actor {

    /** An object that can trigger a callback at (approximately) a specific future time. */
    class Timer {
    public:
        using clock = std::chrono::steady_clock;
        using time = clock::time_point;
        using duration = clock::duration;
        using callback = std::function<void()>;

        /** Constructs a Timer that will call the given callback when it triggers.
            The call happens on an unspecified background thread.
            It should not block, or it will delay all other timers from firing.
            It may call the Timer API, including re-scheduling itself. */
        Timer(callback cb)              :_callback(cb) { }

        /** Destructs a timer. If the timer was scheduled, and the destructor is called just as
            it fires, it is possible for the callback to be running (on another thread) while this
            thread is in the destructor; but in that case the destructor will not return until 
            after the callback completes. */
        ~Timer()                        {manager().unschedule(this, true);}

        /** Calling this causes the Timer to be deleted after it's fired. */
        void autoDelete()               {_autoDelete = true;}

        /** Schedules the timer to fire at the given time (or slightly later.)
            If it was already scheduled, its fire time will be changed.
            If the fire time is now or in the past, the callback will be called ASAP. */
        void fireAt(time t)             {manager().setFireTime(this, t);}

        /** Schedules the timer to fire _earlier_ than it otherwise would.
            If the timer is already scheduled, and its fire time is before `t`, nothing changes.
            Otherwise it's the same as calling `fireAt(t)`. */
        bool fireEarlierAt(time t)      {return manager().setFireTime(this, t, true);}

        /** Schedules the timer to fire after the given duration from the current time.
            (This just converts the duration to an absolute time_point and calls fireAt().)
            If the duration is zero, the callback will be called ASAP. */
        void fireAfter(duration d)      {manager().setFireTime(this, clock::now() + d);}

        /** Schedules the timer to fire _earlier_ than it otherwise would.
            If the timer is already scheduled, and will fire before `d` elapses, nothing changes.
            Otherwise it's the same as calling `fireAfter(d)`. */
        bool fireEarlierAfter(duration d) {return fireEarlierAt(clock::now() + d);}

        template<class Rep, class Period>
        void fireAfter(const std::chrono::duration<Rep,Period>& dur) {
            return fireAfter(std::chrono::duration_cast<duration>(dur));
        }

        /** Unschedules the timer. After this call returns the callback will NOT be invoked
            unless fireAt() or fireAfter() are called. */
        void stop()                     {if (scheduled()) manager().unschedule(this);}

        /** Is the timer active: waiting to fire or in the act of firing? */
        bool scheduled() const          {return _state == kScheduled || _triggered;}

    private:

        enum state : uint8_t {
            kUnscheduled,               // Idle
            kScheduled,                 // In _scheduled queue, waiting to fire
            kDeleted,                   // Destructor called, waiting for fire to complete
        };

        /** Internal singleton that tracks all scheduled Timers and runs a background thread. */
        class Manager {
        public:
            using map = std::multimap<time, Timer*>;
            
            Manager();
            bool setFireTime(Timer*, time, bool ifEarlier =false);
            void unschedule(Timer*, bool deleting =false);
            
        private:
            bool _unschedule(Timer*);
            void run();

            map _schedule;                      // A priority queue of Timers ordered by time
            std::mutex _mutex;                  // Thread-safety for _schedule
            std::condition_variable _condition; // Used to signal that _schedule has changed
            std::thread _thread;                // Bg thread that waits & fires Timers
        };

        friend class Manager;
        static Manager& manager();

        callback _callback;                     // The function to call when I fire
        time _fireTime;                         // Absolute time that I fire
        std::atomic<state> _state {kUnscheduled};   // Current state
        std::atomic<bool> _triggered {false};   // True while callback is being called
        bool _autoDelete {false};               // If true, delete after firing
        Manager::map::iterator _entry;          // My map entry in Manager::_schedule
    };

} }
