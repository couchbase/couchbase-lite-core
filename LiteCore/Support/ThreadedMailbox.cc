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
#    include "Actor.hh"
#    include "ThreadUtil.hh"
#    include "Error.hh"
#    include "Timer.hh"
#    include "Logging.hh"
#    include <future>
#    include <random>
#    include <map>
#    include <sstream>

#    define THREAD_STATS
#    ifdef THREAD_STATS
#        include "Logging_Internal.hh"
#    endif
namespace { namespace threadStats {
#    ifdef THREAD_STATS
        struct Stats {
            uint64_t                timestamp;  // microseconds
            litecore::actor::Actor* actor;
        };

        std::vector<Stats> enterTimes;

        constexpr unsigned warningThreshold = 1000000;  // 1s
        constexpr unsigned checkInterval    = warningThreshold;

        uint64_t   lastChecked = 0;
        std::mutex mutex;
        using namespace std::chrono;
        using namespace litecore;

        void init(size_t numThreads) { enterTimes.resize(numThreads); }

        void enter(size_t taskID, actor::Actor* actor) {
            uint64_t        timestamp = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
            std::lock_guard lock(mutex);
            enterTimes[taskID - 1] = {timestamp, actor};
        }

        void exit(size_t taskID) {
            std::lock_guard lock(mutex);
            enterTimes[taskID - 1] = {0, nullptr};
        }

        void check() {
            uint64_t timestamp = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
            if ( timestamp - lastChecked < checkInterval ) {
                return;
            } else
                lastChecked = timestamp;

            std::vector<std::pair<actor::Actor*, uint64_t>> longRunningActors;
            {
                std::lock_guard lock(mutex);
                for ( size_t i = 0; i < enterTimes.size(); ++i ) {
                    if ( enterTimes[i].timestamp == 0 ) continue;
                    int64_t elapsed = timestamp - enterTimes[i].timestamp;
                    if ( elapsed > warningThreshold ) longRunningActors.push_back({enterTimes[i].actor, elapsed});
                }
            }
            if ( longRunningActors.size() == 0 ) return;

            bool              warning = false;
            std::stringstream ss;
            ss << "Busy threads: ";
            if ( longRunningActors.size() < enterTimes.size() )
                ss << longRunningActors.size() << " out of " << enterTimes.size()
                   << " threads are occupied by actors for excessive amount of time:\n";
            else {
                ss << "all " << enterTimes.size() << " threads are occupied by actors for excessive amount of time:\n";
                warning = true;
            }

            for ( size_t i = 0; i < longRunningActors.size(); ++i ) {
                std::string objPath;
                if ( LogObjectRef objRef = longRunningActors[i].first->getObjectRef(); objRef != LogObjectRef::None ) {
                    objPath = loginternal::sObjectMap.getObjectPath(objRef);
                }
                ss << "  actor=";
                if ( objPath.empty() ) ss << longRunningActors[i].first;
                else
                    ss << objPath;
                ss << " timeInThread=" << longRunningActors[i].second / 1000.0 << "ms";
                if ( i + 1 < longRunningActors.size() ) ss << "\n";
            }
            if ( warning ) LogWarn(litecore::ActorLog, "%s", ss.str().c_str());
            else
                LogTo(litecore::ActorLog, "%s", ss.str().c_str());
        }
#    else
        void init(size_t numThreads) {}

        void enter(size_t taskID, litecore::actor::Actor* actor) {}

        void exit(size_t taskID) {}

        void check() {}
#    endif
}}  // namespace ::threadStats

using namespace std;

namespace litecore { namespace actor {

#    if ACTORS_TRACK_STATS
#        define beginLatency() fleece::Stopwatch st
#        define endLatency()   _maxLatency = max(_maxLatency, (double)st.elapsed())
#        define beginBusy()    _busy.start()
#        define endBusy()      _busy.stop()
#        define SELF           this, st
#    else
#        define beginLatency()
#        define endLatency()
#        define beginBusy()
#        define endBusy()
#        define SELF this
#    endif

#    pragma mark - SCHEDULER:

        struct RunAsyncActor : Actor {
            RunAsyncActor() : Actor(kC4Cpp_DefaultLog, "runAsync") {}

            void runAsync(void (*task)(void*), void* context) {
                enqueue(FUNCTION_TO_QUEUE(RunAsyncActor::_runAsync), task, context);
            }

          private:
            void _runAsync(void (*task)(void*), void* context) { task(context); }
        };

        static Scheduler*    sScheduler;
        static random_device rd;
        static mt19937       sRandGen(rd());

        Scheduler* Scheduler::sharedScheduler() {
            if ( !sScheduler ) {
                sScheduler = new Scheduler;
                sScheduler->start();
            }
            return sScheduler;
        }

        void Scheduler::start() {
            if ( !_started.test_and_set() ) {
                if ( _numThreads == 0 ) {
                    _numThreads = thread::hardware_concurrency();
                    if ( _numThreads == 0 ) _numThreads = 2;
                }
                LogTo(ActorLog, "Starting Scheduler<%p> with %u threads", this, _numThreads);
                threadStats::init(_numThreads);
                for ( unsigned id = 1; id <= _numThreads; id++ ) _threadPool.emplace_back([this, id] { task(id); });
            }
        }

        void Scheduler::stop() {
            LogTo(ActorLog, "Stopping Scheduler<%p>...", this);
            _queue.close();
            for ( auto& t : _threadPool ) { t.join(); }
            LogTo(ActorLog, "Scheduler<%p> has stopped", this);
            _started.clear();
        }

        void Scheduler::task(unsigned taskID) {
            LogVerbose(ActorLog, "   task %d starting", taskID);
            constexpr size_t bufSize = 100;
            char             name[bufSize];
            snprintf(name, bufSize, "CBL Scheduler#%u", taskID);
            SetThreadName(name);
            ThreadedMailbox* mailbox;
            while ( (mailbox = _queue.pop()) != nullptr ) {
                LogVerbose(ActorLog, "   task %d calling Actor<%p>", taskID, mailbox);
                threadStats::enter(taskID, mailbox->_actor);

                mailbox->performNextMessage();

                threadStats::exit(taskID);
                mailbox = nullptr;
            }
            LogTo(ActorLog, "   task %d finished", taskID);
        }

        void Scheduler::schedule(ThreadedMailbox* mbox) { sScheduler->_queue.push(mbox); }


        // Explicitly instantiate the Channel specializations we need; this corresponds to the
        // "extern template..." declarations at the bottom of Actor.hh
        template class Channel<ThreadedMailbox*>;
        template class Channel<std::function<void()>>;


#    pragma mark - MAILBOX:

        thread_local Actor* ThreadedMailbox::sCurrentActor;

#    if ACTORS_USE_MANIFESTS
        thread_local shared_ptr<ChannelManifest> ThreadedMailbox::sThreadManifest;
#    endif

        ThreadedMailbox::ThreadedMailbox(Actor* a, const std::string& name, ThreadedMailbox* parent)
            : _actor(a), _name(name) {
            Scheduler::sharedScheduler()->start();
        }

        void ThreadedMailbox::enqueue(const char* name, const std::function<void()>& f) {
            beginLatency();
            retain(_actor);
            threadStats::check();

#    if ACTORS_USE_MANIFESTS
            auto threadManifest = sThreadManifest ? sThreadManifest : make_shared<ChannelManifest>();
            threadManifest->addEnqueueCall(_actor, name);
            _localManifest.addEnqueueCall(_actor, name);
            const auto wrappedBlock = [f, threadManifest, name, SELF] {
                threadManifest->addExecution(_actor, name);
                sThreadManifest = threadManifest;
                _localManifest.addExecution(_actor, name);
#    else
            const auto wrappedBlock = [f, SELF] {
#    endif
                endLatency();
                beginBusy();
                safelyCall(f);
                afterEvent();

#    if ACTORS_USE_MANIFESTS
                sThreadManifest.reset();
#    endif
            };

            if ( push(wrappedBlock) ) reschedule();
        }

        void ThreadedMailbox::enqueueAfter(delay_t delay, const char* name, const std::function<void()>& f) {
            if ( delay <= delay_t::zero() ) return enqueue(name, f);

            beginLatency();
            _delayedEventCount++;
            retain(_actor);
            threadStats::check();

#    if ACTORS_USE_MANIFESTS
            auto threadManifest = sThreadManifest ? sThreadManifest : make_shared<ChannelManifest>();
            threadManifest->addEnqueueCall(_actor, name, delay.count());
            _localManifest.addEnqueueCall(_actor, name, delay.count());
            auto timer = new Timer([f, threadManifest, name, this] {
                const auto wrappedBlock = [f, threadManifest, name, SELF] {
                    threadManifest->addExecution(_actor, name);
                    sThreadManifest = threadManifest;
                    _localManifest.addExecution(_actor, name);
#    else
            auto       timer        = new Timer([f, this] {
                const auto wrappedBlock = [f, SELF] {
#    endif
                    endLatency();
                    beginBusy();
                    safelyCall(f);
                    --_delayedEventCount;
                    afterEvent();
#    if ACTORS_USE_MANIFESTS
                    sThreadManifest.reset();
#    endif
                };

                if ( push(wrappedBlock) ) reschedule();
            });

            timer->autoDelete();
            timer->fireAfter(chrono::duration_cast<Timer::duration>(delay));
        }

        void ThreadedMailbox::safelyCall(const std::function<void()>& f) const {
            try {
                f();
            } catch ( std::exception& x ) {
                _actor->caughtException(x);
#    if ACTORS_USE_MANIFESTS
                stringstream manifest;
                manifest << "Thread Manifest History:" << endl;
                sThreadManifest->dump(manifest);
                manifest << endl << "Actor Manifest History:" << endl;
                _localManifest.dump(manifest);
                const auto dumped = manifest.str();
                Warn("%s", dumped.c_str());
#    endif
            }
        }

        void ThreadedMailbox::afterEvent() {
            _actor->afterEvent();
            endBusy();

#    if ACTORS_TRACK_STATS
            ++_callCount;
            if ( eventCount() > _maxEventCount ) { _maxEventCount = eventCount(); }
#    endif
        }

        void ThreadedMailbox::reschedule() { Scheduler::schedule(this); }

        void ThreadedMailbox::performNextMessage() {
            LogVerbose(ActorLog, "%s performNextMessage", _actor->actorName().c_str());
            DebugAssert(++_active == 1);  // Fail-safe check to detect 'impossible' re-entrant call
            sCurrentActor = _actor;
            auto& fn      = front();
            fn();
            sCurrentActor = nullptr;

            DebugAssert(--_active == 0);

            bool empty;
            popNoWaiting(empty);
            release(_actor);  // For enqueue's retain call
            if ( !empty ) reschedule();
        }

        void ThreadedMailbox::logStats() const {
#    if ACTORS_TRACK_STATS
            LogTo(ActorLog, "%s handled %d events; max queue depth was %d; max latency was %s; busy %s (%.1f%%)",
                  _actor->actorName().c_str(), _callCount, _maxEventCount,
                  fleece::Stopwatch::formatTime(_maxLatency).c_str(),
                  fleece::Stopwatch::formatTime(_busy.elapsed()).c_str(),
                  (_busy.elapsed() / _createdAt.elapsed()) * 100.0);
#    endif
        }

        void ThreadedMailbox::runAsyncTask(void (*task)(void*), void* context) {
            static RunAsyncActor* sRunAsyncActor =
                    retain(new RunAsyncActor());  // I grant unto thee the gift of eternal life
            sRunAsyncActor->runAsync(task, context);
        }


}}  // namespace litecore::actor
#endif
