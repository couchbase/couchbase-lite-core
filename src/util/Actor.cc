//
//  Actor.cc
//  Actors
//
//  Created by Jens Alfke on 1/5/17.
//
//

#include "Actor.hh"
#include "Channel.cc"       // Brings in the definitions of the template methods
#include "Timer.hh"
#include "Logging.hh"

using namespace std;

namespace litecore {

    LogDomain ActorLog("Actor");


    // Explicitly instantiate the Channel specializations we need; this corresponds to the
    // "extern template..." declarations at the bottom of Actor.hh
    template class Channel<ThreadedMailbox*>;
    template class Channel<std::function<void()>>;


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
#ifndef MSC_VER
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


#pragma mark - MAILBOX:


#ifdef ACTORS_USE_GCD

    GCDMailbox::GCDMailbox(Actor *a, const std::string &name, Scheduler *s)
    :_actor(a),
     _queue(dispatch_queue_create((name.empty() ? nullptr : name.c_str()), DISPATCH_QUEUE_SERIAL))
    { }

    GCDMailbox::~GCDMailbox() {
        dispatch_release(_queue);
    }


    std::string GCDMailbox::name() const {
        return dispatch_queue_get_label(_queue);
    }


    void GCDMailbox::enqueue(std::function<void()> f) {
        ++_eventCount;
        retain(_actor);
        dispatch_async(_queue, ^{
            f();
            _actor->afterEvent();
            release(_actor);
            --_eventCount;
        });
    }


    void GCDMailbox::enqueue(void (^block)()) {
        ++_eventCount;
        retain(_actor);
        auto wrappedBlock = ^{
            block();
            _actor->afterEvent();
            --_eventCount;
            release(_actor);
        };
        dispatch_async(_queue, wrappedBlock);
    }


    void GCDMailbox::enqueueAfter(Scheduler::duration delay, void (^block)()) {
        ++_eventCount;
        retain(_actor);
        auto wrappedBlock = ^{
            block();
            _actor->afterEvent();
            --_eventCount;
            release(_actor);
        };
        int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();
        if (ns > 0)
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ns), _queue, wrappedBlock);
        else
            dispatch_async(_queue, wrappedBlock);
    }


    void GCDMailbox::enqueueAfter(Scheduler::duration delay, std::function<void()> f) {
        ++_eventCount;
        retain(_actor);
        auto wrappedBlock = ^{
            f();
            _actor->afterEvent();
            --_eventCount;
            release(_actor);
        };
        int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();
        if (ns > 0)
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ns), _queue, wrappedBlock);
        else
            dispatch_async(_queue, wrappedBlock);
    }


#endif // ACTORS_USE_GCD


    ThreadedMailbox::ThreadedMailbox(Actor *a, const std::string &name)
    :_actor(a)
    ,_name(name)
    {
        Scheduler::sharedScheduler()->start();
    }


    void ThreadedMailbox::reschedule() {
        sScheduler->schedule(this);
    }



    void ThreadedMailbox::enqueue(std::function<void()> f) {
        retain(_actor);
        if (push(f))
            reschedule();
    }


    void ThreadedMailbox::enqueueAfter(Scheduler::duration delay, std::function<void()> f) {
#if 1
        enqueue(f);
#else
        if (delay == Scheduler::duration::zero())
            return enqueue(f);
        Timer *timer = new Timer([=]{
            enqueue(f);
        });
        timer->autoDelete();
        timer->fireAfter(chrono::duration_cast<Timer::duration>(delay));
#endif
    }


    void ThreadedMailbox::performNextMessage() {
        LogTo(ActorLog, "%s performNextMessage", _actor->actorName().c_str());
#if DEBUG
        assert(++_active == 1);     // Fail-safe check to detect 'impossible' re-entrant call
#endif
        try {
            auto &fn = front();
            fn();
        } catch (...) {
            Warn("EXCEPTION thrown from actor method");
        }
        _actor->afterEvent();
        
#if DEBUG
        assert(--_active == 0);
#endif

        bool empty;
        pop(empty);
        if (!empty)
            reschedule();
        release(_actor);
    }

}
