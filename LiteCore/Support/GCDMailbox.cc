//
// GCDMailbox.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "GCDMailbox.hh"
#include "Actor.hh"
#include "Logging.hh"
#include <algorithm>
#include <sstream>
#include "betterassert.hh"

using namespace std;

namespace litecore { namespace actor {


#if ACTORS_TRACK_STATS
#define beginLatency()  fleece::Stopwatch st
#define endLatency()    _maxLatency = max(_maxLatency, (double)st.elapsed())
#define beginBusy()     _busy.start()
#define endBusy()       _maxBusy = max(_maxBusy, _busy.lap())
#else
#define beginLatency()  ({})
#define endLatency()    ({})
#define beginBusy()     ({})
#define endBusy()       ({})
#endif

    
    static char kQueueMailboxSpecificKey;
    static char kQueueManifestKey;

    static std::shared_ptr<ChannelManifest> get_queue_manifest(dispatch_queue_t queue) {
        auto* existing = (std::shared_ptr<ChannelManifest>*)dispatch_queue_get_specific(queue, &kQueueManifestKey);
        if(existing) {
            return *existing;
        }
        
        return make_shared<ChannelManifest>();
    }

    static const qos_class_t kQOS = QOS_CLASS_UTILITY;

    GCDMailbox::GCDMailbox(Actor *a, const std::string &name, GCDMailbox *parentMailbox)
    :_actor(a)
    {
        dispatch_queue_t targetQueue;
        if (parentMailbox)
            targetQueue = parentMailbox->_queue;
        else
            targetQueue = dispatch_get_global_queue(kQOS, 0);
        auto nameCstr = name.empty() ? nullptr : name.c_str();
        dispatch_queue_attr_t attr = DISPATCH_QUEUE_SERIAL;
        attr = dispatch_queue_attr_make_with_qos_class(attr, kQOS, 0);
        attr = dispatch_queue_attr_make_with_autorelease_frequency(attr,
                                                        DISPATCH_AUTORELEASE_FREQUENCY_NEVER);
        _queue = dispatch_queue_create_with_target(nameCstr, attr, targetQueue);
        dispatch_queue_set_specific(_queue, &kQueueMailboxSpecificKey, this, nullptr);
    }

    GCDMailbox::~GCDMailbox() {
        dispatch_release(_queue);
    }


    std::string GCDMailbox::name() const {
        return dispatch_queue_get_label(_queue);
    }


    Actor* GCDMailbox::currentActor() {
        auto mailbox = (GCDMailbox*) dispatch_get_specific(&kQueueMailboxSpecificKey);
        return mailbox ? mailbox->_actor : nullptr;
    }


    void GCDMailbox::safelyCall(void (^block)()) const {
        try {
            block();
        } catch (const std::exception &x) {
            _actor->caughtException(x);
#if ACTORS_USE_MANIFESTS
            stringstream manifest;
            manifest << "Queue Manifest History:" << endl;
            auto queueManifest = get_queue_manifest(_queue);
            queueManifest->dump(manifest);
            manifest << endl << "Actor Manifest History:" << endl;
            _localManifest.dump(manifest);
            const auto dumped = manifest.str();
            Warn("%s", dumped.c_str());
#endif
        }
    }

    
    void GCDMailbox::enqueue(const char* name, void (^block)()) {
        beginLatency();
        ++_eventCount;
        retain(_actor);

#if ACTORS_USE_MANIFESTS
        // Either queueManifest is set (see below inside of wrappedBlock) or this is a top
        // level call to enqueue and a new queueManifest needs to be created
        auto queueManifest = get_queue_manifest(_queue);
        queueManifest->addEnqueueCall(_actor, name);
        _localManifest.addEnqueueCall(_actor, name);
#endif
        
        auto wrappedBlock = ^{
#if ACTORS_USE_MANIFESTS
            queueManifest->addExecution(_actor, name);

            // Set the captured queue manifest to be the current queue manifest so that
            // any calls to enqueue inside of safelyCall() will use the same one (see above)
            dispatch_queue_set_specific(_queue, &kQueueManifestKey, (void *)&queueManifest, nullptr);
            _localManifest.addExecution(_actor, name);
#endif
            endLatency();
            beginBusy();
            safelyCall(block);
            afterEvent();
#if ACTORS_USE_MANIFESTS
            dispatch_queue_set_specific(_queue, &kQueueManifestKey, nullptr, nullptr);
#endif
        };
        dispatch_async(_queue, wrappedBlock);
    }


    void GCDMailbox::enqueueAfter(delay_t delay, const char* name, void (^block)()) {
        beginLatency();
        ++_eventCount;
        retain(_actor);

#if ACTORS_USE_MANIFESTS
        // Either queueManifest is set (see below inside of wrappedBlock) or this is a top
        // level call to enqueueAfter and a new queueManifest needs to be created
        auto queueManifest = get_queue_manifest(_queue);
        queueManifest->addEnqueueCall(_actor, name, delay.count());
        _localManifest.addEnqueueCall(_actor, name, delay.count());
#endif
        
        auto wrappedBlock = ^{
#if ACTORS_USE_MANIFESTS
            queueManifest->addExecution(_actor, name);

            // Set the captured queue manifest to be the queue manifest so that
            // any calls to enqueue inside of safelyCall() will use the same one (see above)
            dispatch_queue_set_specific(_queue, &kQueueManifestKey, (void *)&queueManifest, nullptr);
            _localManifest.addExecution(_actor, name);
#endif
            endLatency();
            beginBusy();
            safelyCall(block);
            afterEvent();
#if ACTORS_USE_MANIFESTS
            dispatch_queue_set_specific(_queue, &kQueueManifestKey, nullptr, nullptr);
#endif
        };
        int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();
        if (ns > 0)
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ns), _queue, wrappedBlock);
        else
            dispatch_async(_queue, wrappedBlock);
    }

    void GCDMailbox::afterEvent() {
        _actor->afterEvent();
        endBusy();
#if ACTORS_TRACK_STATS
        ++_callCount;
        if (_eventCount > _maxEventCount) {
            _maxEventCount = _eventCount;
        }
#endif
        --_eventCount;
        release(_actor);
    }


    void GCDMailbox::logStats() const {
#if ACTORS_TRACK_STATS
        printf("%-25s handled %5d events; max queue depth was %3d; max latency was %10s; busy total %10s (%4.1f%%), max %10s\n",
              _actor->actorName().c_str(), _callCount, _maxEventCount,
              fleece::Stopwatch::formatTime(_maxLatency).c_str(),
              fleece::Stopwatch::formatTime(_busy.elapsed()).c_str(),
              (_busy.elapsed() / _createdAt.elapsed())*100.0,
              fleece::Stopwatch::formatTime(_maxBusy).c_str());
#endif
    }


    void GCDMailbox::runAsyncTask(void (*task)(void*), void *context) {
        static dispatch_queue_t sAsyncTaskQueue;
        static once_flag once;
        call_once(once, [] {
            dispatch_queue_attr_t attr = DISPATCH_QUEUE_CONCURRENT;
            attr = dispatch_queue_attr_make_with_qos_class(attr, QOS_CLASS_BACKGROUND, 0);
            sAsyncTaskQueue = dispatch_queue_create("CBL Async Tasks", attr);
        });
        
        dispatch_async_f(sAsyncTaskQueue, context, task);
    }


} }
