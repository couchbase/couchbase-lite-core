//
//  Actor.cc
//  Actors
//
//  Created by Jens Alfke on 1/5/17.
//
//

#include "Actor.hh"
#include "Channel.cc"       // Brings in the definitions of the template methods
#include "Logging.hh"

using namespace std;

namespace litecore {

    LogDomain ActorLog("Actor");


    // Explicitly instantiate the Channel specializations we need; this corresponds to the
    // "extern template..." declarations at the bottom of Actor.hh
    template class Channel<ThreadedMailbox*>;
    template class Channel<std::function<void()>>;


    Scheduler* Scheduler::sharedScheduler() {
        static Scheduler *sSched = new Scheduler;
        return sSched;
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
        LogTo(ActorLog, "   task %d starting", taskID);
        ThreadedMailbox *mailbox;
        while ((mailbox = _queue.pop()) != nullptr) {
            LogTo(ActorLog, "   task %d calling Actor<%p>", taskID, mailbox);
            mailbox->performNextMessage();
            mailbox = nullptr;
        }
        LogTo(ActorLog, "   task %d finished", taskID);
    }


#ifdef ACTORS_USE_GCD

    GCDMailbox::GCDMailbox(Actor *a, const std::string &name, Scheduler *s)
    :_actor(a),
     _queue(dispatch_queue_create((name.empty() ? nullptr : name.c_str()), DISPATCH_QUEUE_SERIAL))
    { }

    GCDMailbox::~GCDMailbox() {
        dispatch_release(_queue);
    }


    void GCDMailbox::enqueue(std::function<void()> f) {
        retain(_actor);
        dispatch_async(_queue, ^{ f(); release(_actor); });
    }


#endif // ACTORS_USE_GCD


    void ThreadedMailbox::setScheduler(Scheduler *s) {
        assert(s);
        assert(!_scheduler);
        _scheduler = s;
        if (!empty())
            reschedule();
    }


    void ThreadedMailbox::enqueue(std::function<void()> f) {
        retain(_actor);
        if (push(f))
            if (_scheduler)
                reschedule();
    }


    void ThreadedMailbox::performNextMessage() {
        LogTo(ActorLog, "Mailbox<%p> performNextMessage", this);
#if DEBUG
        assert(++_active == 1);     // Fail-safe check to detect 'impossible' re-entrant call
#endif
        try {
            front()();
        } catch (...) {
            Warn("EXCEPTION thrown from actor method");
        }
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
