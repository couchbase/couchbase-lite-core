//
// Batcher.hh
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Logging.hh"
#include "Timer.hh"
#include <atomic>
#include <climits>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace litecore::actor {
    using fleece::Retained;
    static constexpr int AnyGen = INT_MAX;

    /** A simple queue that adds objects one at a time and sends them to its target in a batch. */
    template <class ITEM>
    class Batcher {
      public:
        using Items = std::unique_ptr<std::vector<Retained<ITEM>>>;

        Batcher(std::function<void(int gen)> processNow, std::function<void(int gen)> processLater,
                Timer::duration latency = {}, size_t capacity = 0)
            : _processNow(std::move(processNow))
            , _processLater(std::move(processLater))
            , _latency(latency)
            , _capacity(capacity) {}

        /** Adds an item to the queue, and schedules a call to the Actor if necessary.
            Thread-safe. */
        void push(ITEM* item) {
            std::lock_guard<std::mutex> lock(_mutex);

            if ( !_items ) {
                _items.reset(new std::vector<Retained<ITEM>>);
                _items->reserve(_capacity ? _capacity : 200);
            }
            _items->push_back(item);
            if ( !_scheduled ) {
                // Schedule a pop as soon as an item is added:
                _scheduled = true;
                _processLater(_generation);
            }
            if ( _latency > Timer::duration(0) && _capacity > 0 && _items->size() == _capacity ) {
                // I'm full -- schedule a pop NOW
                LogVerbose(SyncLog, "Batcher scheduling immediate pop");
                _processNow(_generation);
            }
        }

        /** Removes & returns all the items from  the queue, in the order they were added,
            or nullptr if nothing has been added to the queue.
            Thread-safe. */
        Items pop(int gen = AnyGen) {
            std::lock_guard<std::mutex> lock(_mutex);

            if ( gen < _generation ) return {};
            _scheduled = false;
            ++_generation;
            return std::move(_items);
        }

      private:
        std::function<void(int gen)> _processNow, _processLater;
        Timer::duration              _latency;
        size_t                       _capacity;
        std::mutex                   _mutex;
        Items                        _items{};
        int                          _generation{0};
        bool                         _scheduled{false};
    };

    /** A simple queue that adds objects one at a time and sends them to an Actor in a batch. */
    template <class ACTOR, class ITEM>
    class ActorBatcher : public Batcher<ITEM> {
      public:
        typedef void (ACTOR::*Processor)(int gen);

        /** Constructs an ActorBatcher. Typically done in the Actor subclass's constructor.
            @param actor  The Actor that owns this queue.
            @param processor  The Actor method that should be called to process the queue.
            @param latency  How long to wait before calling the processor, after the first item
                            is added to the queue. */
        ActorBatcher(ACTOR* actor, const char* name, Processor processor, Timer::duration latency = {},
                     size_t capacity = 0)
            : Batcher<ITEM>([=](int gen) { actor->enqueue(_name, processor, gen); },
                            [=](int gen) { actor->enqueueAfter(latency, _name, processor, gen); }, latency, capacity)
            , _name(name) {}

      private:
        const char* _name;
    };

    class CountBatcher {
      public:
        explicit CountBatcher(std::function<void()> process) : _process(std::move(process)) {}

        /** Adds to the count. If the count was zero, it calls the process function. */
        void add(unsigned n = 1) {
            if ( _count.fetch_add(n) == 0 ) _process();
        }

        /** Returns the count and resets it to zero. */
        unsigned take() { return _count.exchange(0); }

      private:
        std::function<void()> _process;
        std::atomic<unsigned> _count{0};
    };

    template <class ACTOR>
    class ActorCountBatcher : public CountBatcher {
      public:
        typedef void (ACTOR::*Processor)();

        ActorCountBatcher(ACTOR* actor, const char* name, Processor processor)
            : CountBatcher([=]() { actor->enqueue(_name, processor); }), _name(name) {}

      private:
        const char* _name;
    };


}  // namespace litecore::actor
