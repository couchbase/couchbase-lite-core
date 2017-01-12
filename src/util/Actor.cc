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
    template class Channel<Retained<Actor>>;
    template class Channel<std::function<void()>>;



    void Scheduler::start(unsigned numThreads) {
        if (numThreads == 0) {
            numThreads = thread::hardware_concurrency();
            if (numThreads == 0)
                numThreads = 2;
        }
        LogTo(ActorLog, "Starting Scheduler<%p> with %u threads", this, numThreads);
        for (unsigned id = 1; id <= numThreads; id++)
            _threadPool.emplace_back([this,id]{task(id);});
    }

    void Scheduler::stop() {
        LogTo(ActorLog, "Stopping Scheduler<%p>...", this);
        _queue.close();
        for (auto &t : _threadPool) {
            t.join();
        }
        LogTo(ActorLog, "Scheduler<%p> has stopped", this);
    }

    void Scheduler::task(unsigned taskID) {
        LogTo(ActorLog, "   task %d starting", taskID);
        Retained<Actor> actor;
        while ((actor = _queue.pop()) != nullptr) {
            LogTo(ActorLog, "   task %d calling Actor<%p>", taskID, actor.get());
            actor->performNextMessage();
            actor = nullptr;
        }
        LogTo(ActorLog, "   task %d finished", taskID);
    }



    void Actor::setScheduler(Scheduler *s) {
        assert(s);
        assert(!_scheduler);
        _scheduler = s;
        if (!_mailbox.empty())
            reschedule();
    }


    void Actor::enqueue(std::function<void()> f) {
        if (_mailbox.push(f))
            if (_scheduler)
                reschedule();
    }


    void Actor::performNextMessage() {
        LogTo(ActorLog, "Actor<%p> performNextMessage", this);
        bool nowEmpty;
        try {
            _mailbox.pop(nowEmpty)();
        } catch (...) {
            Warn("EXCEPTION thrown from actor method");
        }
        if (!nowEmpty)
            reschedule();
    }

}
