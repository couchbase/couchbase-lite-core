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
#include "Actor.hh"
#include "Error.hh"
#include "Timer.hh"
#include "Logging.hh"
#include "Channel.cc"       // Brings in the definitions of the template methods

using namespace std;

namespace litecore { namespace actor {

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


#pragma mark - MAILBOX PROXY


    /*  The only purpose of this class is to handle the situation where an enqueueAfter triggers
        after its target Actor has been deleted. It has a weak reference to a mailbox (which is
        cleared by the mailbox's destructor.) The proxy is retained by the Timer's lambda, so
        it can safely be called when the timer fires; it will tell its mailbox to enqueue the
        function, unless the mailbox has been deleted. */
    class MailboxProxy : public RefCounted {
    public:
        MailboxProxy(ThreadedMailbox *m)
        :_mailbox(m)
        { }

        void detach() {
            _mailbox = nullptr;
        }

        void enqueue(function<void()> f) {
            ThreadedMailbox* mb = _mailbox;
            if (mb)
                mb->enqueue(f);
        }

    private:
        virtual ~MailboxProxy() =default;
        atomic<ThreadedMailbox*> _mailbox;
    };


#pragma mark - MAILBOX:

    __thread Actor* ThreadedMailbox::sCurrentActor;


    ThreadedMailbox::ThreadedMailbox(Actor *a, const std::string &name, ThreadedMailbox *parent)
    :_actor(a)
    ,_name(name)
    {
        Scheduler::sharedScheduler()->start();
    }


    ThreadedMailbox::~ThreadedMailbox() {
        if (_proxy)
            _proxy->detach();
    }


    void ThreadedMailbox::enqueue(std::function<void()> f) {
        retain(_actor);
        if (push(f))
            reschedule();
    }


    void ThreadedMailbox::enqueueAfter(delay_t delay, std::function<void()> f) {
        if (delay <= delay_t::zero())
            return enqueue(f);

        auto actor = _actor;
        retain(actor);
        Retained<MailboxProxy> proxy;
        {
            std::lock_guard<mutex> lock(_mutex);
            proxy = _proxy;
            if (!proxy)
                proxy = _proxy = new MailboxProxy(this);
        }

        auto timer = new Timer([proxy, f, actor]
        { 
            proxy->enqueue(f);
            release(actor);
        });
        timer->autoDelete();
        timer->fireAfter(chrono::duration_cast<Timer::duration>(delay));
    }


    void ThreadedMailbox::reschedule() {
        Scheduler::schedule(this);
    }


    void ThreadedMailbox::performNextMessage() {
        LogToAt(ActorLog, Verbose, "%s performNextMessage", _actor->actorName().c_str());
        DebugAssert(++_active == 1);     // Fail-safe check to detect 'impossible' re-entrant call
        sCurrentActor = _actor;
        try {
            auto &fn = front();
            fn();
        } catch (const std::exception &x) {
            _actor->caughtException(x);
        }
        _actor->afterEvent();
        sCurrentActor = nullptr;
        
        DebugAssert(--_active == 0);

        bool empty;
        popNoWaiting(empty);
        release(_actor);
        if (!empty)
            reschedule();
    }

} }
