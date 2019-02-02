//
// ThreadedMailbox.cc
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

#include "ThreadedMailbox.hh"

#ifndef ACTORS_USE_GCD
#include "Actor.hh"
#include "Error.hh"
#include "Timer.hh"
#include "Logging.hh"
#include "Channel.cc"       // Brings in the definitions of the template methods

using namespace std;

namespace litecore { namespace actor {

#if ACTORS_TRACK_STATS
#define beginLatency()  fleece::Stopwatch st
#define endLatency()    _maxLatency = max(_maxLatency, (double)st.elapsed())
#define beginBusy()     _busy.start()
#define endBusy()       _busy.stop()
#define SELF            this, st
#else
#define beginLatency()
#define endLatency()
#define beginBusy()
#define endBusy()
#define SELF            this
#endif

#pragma mark - SCHEDULER:

    
    static Scheduler* sScheduler;


    Scheduler* Scheduler::sharedScheduler() {
        if (!sScheduler) {
            sScheduler = new Scheduler;
            sScheduler->start();
        }
        return sScheduler;
    }


    void Scheduler::start() {
        if (!_started.test_and_set()) {
            if (_numThreads == 0) {
                _numThreads = thread::hardware_concurrency();
                if (_numThreads == 0)
                    _numThreads = 2;
            }
            LogTo(ActorLog, "Starting Scheduler<%p> with %u threads", this, _numThreads);
            for (unsigned id = 1; id <= _numThreads; id++)
                _threadPool.emplace_back([this,id]{task(id);});
        }
    }
    

    void Scheduler::stop() {
        LogTo(ActorLog, "Stopping Scheduler<%p>...", this);
        _queue.close();
        for (auto &t : _threadPool) {
            t.join();
        }
        LogTo(ActorLog, "Scheduler<%p> has stopped", this);
        _started.clear();
    }


    void Scheduler::task(unsigned taskID) {
        LogToAt(ActorLog, Verbose, "   task %d starting", taskID);
#ifndef _MSC_VER
        {
            char name[100];
            sprintf(name, "LiteCore Scheduler #%u", taskID);
#ifdef __APPLE__
            pthread_setname_np(name);
#else
            pthread_setname_np(pthread_self(), name);
#endif
        }
#endif
        ThreadedMailbox *mailbox;
        while ((mailbox = _queue.pop()) != nullptr) {
            LogToAt(ActorLog, Verbose, "   task %d calling Actor<%p>", taskID, mailbox);
            mailbox->performNextMessage();
            mailbox = nullptr;
        }
        LogTo(ActorLog, "   task %d finished", taskID);
    }


    void Scheduler::schedule(ThreadedMailbox *mbox) {
        sScheduler->_queue.push(mbox);
    }


    // Explicitly instantiate the Channel specializations we need; this corresponds to the
    // "extern template..." declarations at the bottom of Actor.hh
    template class Channel<ThreadedMailbox*>;
    template class Channel<std::function<void()>>;


#pragma mark - MAILBOX:

    thread_local Actor* ThreadedMailbox::sCurrentActor;


    ThreadedMailbox::ThreadedMailbox(Actor *a, const std::string &name, ThreadedMailbox *parent)
    :_actor(a)
    ,_name(name)
    {
        Scheduler::sharedScheduler()->start();
    }

    void ThreadedMailbox::enqueue(std::function<void()> f) {
        beginLatency();
        retain(_actor);
        const auto wrappedBlock = [f, SELF]
        {
            endLatency();
            beginBusy();
            safelyCall(f);
            afterEvent(false);
        };

        if (push(wrappedBlock))
            reschedule();
    }

    void ThreadedMailbox::enqueueAfter(delay_t delay, std::function<void()> f) {
        if (delay <= delay_t::zero())
            return enqueue(f);

        beginLatency();
        _delayedEventCount++;
        retain(_actor);

        auto timer = new Timer([f, this]
        { 
            const auto wrappedBlock = [f, SELF]
            {
                endLatency();
                beginBusy();
                safelyCall(f);
                afterEvent(true);
            };
            enqueue(wrappedBlock);
        });

        timer->autoDelete();
        timer->fireAfter(chrono::duration_cast<Timer::duration>(delay));
    }

    void ThreadedMailbox::safelyCall(const std::function<void()>& f) const
    {
        try {
            f();
        } catch(std::exception& x) {
            _actor->caughtException(x);
        }
    }

    void ThreadedMailbox::afterEvent(bool wasDelayed)
    {
#if ACTORS_TRACK_STATS
        ++_callCount;
        if(eventCount() > _maxEventCount) {
            _maxEventCount = eventCount();
        }
#endif

        // This needs to happen *before* the actor's afterEvent call because this flow
        // needs to be protected:
        // 1. enqueueAfter increments _delayedEventCount (eventCount() = 1)
        // 2. After the delay, enqueueAfter calls enqueue (eventCount() = 2)
        // 3. _delayedEventCount needs to be decremented to give the correct value
        //    of eventCount() = 1 during the actor's afterEvent
        // This has to be on the same thread as the mailbox so no races occur
        if(wasDelayed) {
            --_delayedEventCount;
        }

        _actor->afterEvent();
        endBusy();
        release(_actor);
    }


    void ThreadedMailbox::reschedule() {
        Scheduler::schedule(this);
    }


    void ThreadedMailbox::performNextMessage() {
        LogToAt(ActorLog, Verbose, "%s performNextMessage", _actor->actorName().c_str());
        DebugAssert(++_active == 1);     // Fail-safe check to detect 'impossible' re-entrant call
        sCurrentActor = _actor;
        auto &fn = front();
        fn();
        sCurrentActor = nullptr;
        
        DebugAssert(--_active == 0);

        bool empty;
        popNoWaiting(empty);
        if (!empty)
            reschedule();
    }

    void ThreadedMailbox::logStats() const
    {
#if ACTORS_TRACK_STATS
        LogTo(ActorLog, "%s handled %d events; max queue depth was %d; max latency was %s; busy %s (%.1f%%)",
            _actor->actorName().c_str(), _callCount, _maxEventCount,
            fleece::Stopwatch::formatTime(_maxLatency).c_str(),
            fleece::Stopwatch::formatTime(_busy.elapsed()).c_str(),
            (_busy.elapsed() / _createdAt.elapsed()) * 100.0);
#endif
    }


} }
#endif
