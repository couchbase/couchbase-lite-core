//
// ThreadedMailbox.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "ThreadedMailbox.hh"
#ifndef ACTORS_USE_GCD
#include "Actor.hh"
#include "ThreadUtil.hh"
#include "Error.hh"
#include "Timer.hh"
#include "Logging.hh"
#include <future>
#include <random>
#include <map>
#include <sstream>

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

    struct RunAsyncActor : Actor
    {
        RunAsyncActor()
            : Actor(kC4Cpp_DefaultLog, "runAsync") {
        }

        void runAsync(void (*task)(void*), void *context) {
            enqueue(FUNCTION_TO_QUEUE(RunAsyncActor::_runAsync), task, context);
        }

    private:
        void _runAsync(void (*task)(void*), void *context) {
            task(context);
        }
    };
    
    static Scheduler* sScheduler;
    static random_device rd;
    static mt19937 sRandGen(rd());

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
        LogVerbose(ActorLog, "   task %d starting", taskID);
        constexpr size_t bufSize = 100;
        char name[bufSize];
        snprintf(name, bufSize, "CBL Scheduler#%u", taskID);
        SetThreadName(name);
        ThreadedMailbox *mailbox;
        while ((mailbox = _queue.pop()) != nullptr) {
            LogVerbose(ActorLog, "   task %d calling Actor<%p>", taskID, mailbox);
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

#if ACTORS_USE_MANIFESTS
    thread_local shared_ptr<ChannelManifest> ThreadedMailbox::sThreadManifest;
#endif

    ThreadedMailbox::ThreadedMailbox(Actor *a, const std::string &name, ThreadedMailbox *parent)
    :_actor(a)
    ,_name(name)
    {
        Scheduler::sharedScheduler()->start();
    }

    void ThreadedMailbox::enqueue(const char* name, const std::function<void()> &f) {
        beginLatency();
        retain(_actor);

#if ACTORS_USE_MANIFESTS
        auto threadManifest = sThreadManifest ? sThreadManifest : make_shared<ChannelManifest>();
        threadManifest->addEnqueueCall(_actor, name);
        _localManifest.addEnqueueCall(_actor, name);
        const auto wrappedBlock = [f, threadManifest, name, SELF]
        {
            threadManifest->addExecution(_actor, name);
            sThreadManifest = threadManifest;
            _localManifest.addExecution(_actor, name);
#else
        const auto wrappedBlock = [f, SELF]
        {
#endif
            endLatency();
            beginBusy();
            safelyCall(f);
            afterEvent();

#if ACTORS_USE_MANIFESTS
            sThreadManifest.reset();
#endif
        };

        if (push(wrappedBlock))
            reschedule();
    }

    void ThreadedMailbox::enqueueAfter(delay_t delay, const char* name, const std::function<void()> &f) {
        if (delay <= delay_t::zero())
            return enqueue(name, f);

        beginLatency();
        _delayedEventCount++;
        retain(_actor);

#if ACTORS_USE_MANIFESTS
        auto threadManifest = sThreadManifest ? sThreadManifest : make_shared<ChannelManifest>();
        threadManifest->addEnqueueCall(_actor, name, delay.count());
        _localManifest.addEnqueueCall(_actor, name, delay.count());
        auto timer = new Timer([f, threadManifest, name, this]
        {
            const auto wrappedBlock = [f, threadManifest, name, SELF]
            {
                threadManifest->addExecution(_actor, name);
                sThreadManifest = threadManifest;
                _localManifest.addExecution(_actor, name);
#else
        auto timer = new Timer([f, this]
        {
             const auto wrappedBlock = [f, SELF]
             {
#endif
                endLatency();
                beginBusy();
                safelyCall(f);
                --_delayedEventCount;
                afterEvent();
#if ACTORS_USE_MANIFESTS
                sThreadManifest.reset();
#endif
            };
            
            if (push(wrappedBlock))
                reschedule();
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
#if ACTORS_USE_MANIFESTS
            stringstream manifest;
            manifest << "Thread Manifest History:" << endl;
            sThreadManifest->dump(manifest);
            manifest << endl << "Actor Manifest History:" << endl;
            _localManifest.dump(manifest);
            const auto dumped = manifest.str();
            Warn("%s", dumped.c_str());
#endif
        }
    }

    void ThreadedMailbox::afterEvent()
    {
        _actor->afterEvent();
        endBusy();

#if ACTORS_TRACK_STATS
        ++_callCount;
        if(eventCount() > _maxEventCount) {
            _maxEventCount = eventCount();
        }
#endif
    }


    void ThreadedMailbox::reschedule() {
        Scheduler::schedule(this);
    }


    void ThreadedMailbox::performNextMessage() {
        LogVerbose(ActorLog, "%s performNextMessage", _actor->actorName().c_str());
        DebugAssert(++_active == 1);     // Fail-safe check to detect 'impossible' re-entrant call
        sCurrentActor = _actor;
        auto &fn = front();
        fn();
        sCurrentActor = nullptr;
        
        DebugAssert(--_active == 0);

        bool empty;
        popNoWaiting(empty);
        release(_actor); // For enqueue's retain call
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


    void ThreadedMailbox::runAsyncTask(void (*task)(void*), void *context) {
        static RunAsyncActor* sRunAsyncActor = retain(new RunAsyncActor()); // I grant unto thee the gift of eternal life
        sRunAsyncActor->runAsync(task, context);
    }


} }
#endif
