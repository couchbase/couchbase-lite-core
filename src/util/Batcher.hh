//
// Batcher.hh
//
// Copyright © 2018 Couchbase. All rights reserved.
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

#pragma once
#include "Actor.hh"
#include "Logging.hh"
#include "Timer.hh"
#include <climits>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace litecore { namespace actor {

    static constexpr int AnyGen = INT_MAX;

    
    /** A simple queue that adds objects one at a time and sends them to its target in a batch. */
    template <class ITEM>
    class Batcher {
    public:
        using Items = std::unique_ptr<std::vector<Retained<ITEM>>>;

        Batcher(std::function<void(int gen)> processNow,
                std::function<void(int gen)> processLater,
                Timer::duration latency ={},
                size_t capacity = 0)
        :_processNow(processNow)
        ,_processLater(processLater)
        ,_latency(latency)
        ,_capacity(capacity)
        { }

        /** Adds an item to the queue, and schedules a call to the Actor if necessary.
            Thread-safe. */
        void push(ITEM *item) {
            std::lock_guard<std::mutex> lock(_mutex);

            if (!_items) {
                _items.reset(new std::vector<Retained<ITEM>>);
                _items->reserve(_capacity ? _capacity : 200);
            }
            _items->push_back(item);
            if (!_scheduled) {
                // Schedule a pop as soon as an item is added:
                _scheduled = true;
                _processLater(_generation);
            }
            if (_latency > Timer::duration(0) && _capacity > 0 && _items->size() == _capacity) {
                // I'm full -- schedule a pop NOW
                LogVerbose(SyncLog, "Batcher scheduling immediate pop");
                _processNow(_generation);
            }
        }


        /** Removes & returns all the items from  the queue, in the order they were added,
            or nullptr if nothing has been added to the queue.
            Thread-safe. */
        Items pop(int gen =AnyGen) {
            std::lock_guard<std::mutex> lock(_mutex);

            if (gen < _generation)
                return {};
            _scheduled = false;
            ++_generation;
            return move(_items);
        }

    private:
        std::function<void(int gen)> _processNow, _processLater;
        Timer::duration _latency;
        size_t _capacity;
        std::mutex _mutex;
        Items _items;
        int _generation {0};
        bool _scheduled {false};
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
        ActorBatcher(ACTOR *actor,
                     Processor processor,
                     Timer::duration latency ={},
                     size_t capacity = 0)
        :Batcher<ITEM>([=](int gen) {actor->enqueue(processor, gen);},
                       [=](int gen) {actor->enqueueAfter(latency, processor, gen);},
                       latency,
                       capacity)
        { }
    };

} }
