//
//  Timer.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/27/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Timer.hh"
#include <vector>

using namespace std;

namespace litecore {

    Timer::Manager& Timer::manager() {
        static Manager* sManager = new Manager;
        return *sManager;
    }


    Timer::Manager::Manager()
    :_thread([this](){ run(); })
    { }


    void Timer::Manager::run() {
        vector<Timer*> firingSquad;
        while(true) {
            {
                unique_lock<mutex> lock(_mutex);
                // Wait for the first timer's fireTime, or until the _schedule is updated:
                if (_schedule.empty()) {
                    _condition.wait(lock);
                } else {
                    time nextTime = _schedule.begin()->first;
                    _condition.wait_until(lock, nextTime);
                }

                // Find & unschedule timers whose fireTimes have passed:
                time now = clock::now();
                while (!_schedule.empty()) {
                    auto entry = _schedule.begin();
                    if (entry->first > now)
                        break;
                    auto timer = entry->second;
                    _schedule.erase(entry);
                    timer->_triggered = true;
                    timer->_state = kUnscheduled;
                    firingSquad.push_back(timer);
                }
                // ... exiting this block, the mutex is released ...
            }

            // Fire the triggered timers:
            for (auto timer : firingSquad) {
                try {
                    timer->_callback();
                    timer->_triggered = false;                   // note: not holding any lock
                } catch (...) { }
            }
            firingSquad.clear();
        }
    }


    // Waits for a Timer to exit the triggered state (i.e. waits for its callback to complete.)
    void Timer::waitForFire() {
        while (_triggered)
            this_thread::sleep_for(chrono::microseconds(100));
    }


    // Removes a Timer from _scheduled. Returns true if the next fire time is affected.
    // Precondition: _mutex must be locked.
    // Postconditions: timer is not in _scheduled. timer->_state != kScheduled.
    bool Timer::Manager::_unschedule(Timer *timer) {
        while (true) {
            switch ((state)timer->_state) {
                case kUnscheduled:
                    return false;
                case kScheduled: {
                    bool affectsTiming = (timer->_entry == _schedule.begin());
                    _schedule.erase(timer->_entry);
                    timer->_entry = _schedule.end();
                    timer->_state = kUnscheduled;
                    return affectsTiming;
                }
            }
        }
    }


    // Unschedules a timer, preventing it from firing if it hasn't been triggered yet.
    // (Called by Timer::stop())
    // Precondition: _mutex must NOT be locked.
    // Postcondition: timer is not in _scheduled. timer->_state != kScheduled.
    void Timer::Manager::unschedule(Timer *timer) {
        unique_lock<mutex> lock(_mutex);
        if (_unschedule(timer))
            _condition.notify_one();        // wakes up run() so it can recalculate its wait time
    }


    // Schedules or re-schedules a timer. (Called by Timer::fireAt/fireAfter())
    // Precondition: _mutex must NOT be locked.
    // Postcondition: timer is in _scheduled. timer->_state == kScheduled.
    void Timer::Manager::setFireTime(Timer *timer, clock::time_point when) {
        unique_lock<mutex> lock(_mutex);
        bool notify = _unschedule(timer);
        auto result = _schedule.insert({when, timer});
        timer->_entry = result.first;
        timer->_state = kScheduled;
        if (timer->_entry == _schedule.begin() || notify)
            _condition.notify_one();        // wakes up run() so it can recalculate its wait time
    }


}
