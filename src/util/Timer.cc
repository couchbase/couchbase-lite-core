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


    // Body of the manager's background thread. Waits for timers and calls their callbacks.
    void Timer::Manager::run() {
        unique_lock<mutex> lock(_mutex);
        while(true) {
            auto earliest = _schedule.begin();
            if (earliest == _schedule.end()) {
                // Schedule is empty; just wait for a change
                _condition.wait(lock);

            } else if (earliest->first <= clock::now()) {
                // A Timer is ready to fire, so remove it and call the callback:
                auto timer = earliest->second;
                _unschedule(timer);

                // Fire the timer, while not holding the mutex (to avoid deadlocks if the
                // timer callback calls the Timer API.)
                lock.unlock();
                try {
                    timer->_callback();
                } catch (...) { }
                timer->_triggered = false;                   // note: not holding any lock
                lock.lock();

            } else {
                // Wait for the first timer's fireTime, or until the _schedule is updated:
                auto nextFireTime = earliest->first;
                _condition.wait_until(lock, nextFireTime);
            }
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
        if (timer->_state != kScheduled)
            return false;
        bool affectsTiming = (timer->_entry == _schedule.begin());
        _schedule.erase(timer->_entry);
        timer->_entry = _schedule.end();
        timer->_state = kUnscheduled;
        return affectsTiming && !_schedule.empty();
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
