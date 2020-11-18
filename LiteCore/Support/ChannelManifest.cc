//
// ChannelManifest.cc
//
// Copyright (c) 2020 Couchbase, Inc All rights reserved.
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

#include "ChannelManifest.hh"

#if ACTORS_USE_MANIFESTS

#include "ThreadUtil.hh"
#include "Actor.hh"
#include <sstream>
#include <string>
#include <chrono>

using namespace std;
using namespace litecore;
using namespace litecore::actor;

 void ChannelManifest::addEnqueueCall(const Actor* actor, const char* name, double after) {
    auto now = chrono::system_clock::now();
    auto elapsed = chrono::duration_cast<chrono::microseconds>(now - _start);
    stringstream s;
    s << actor->loggingName() << "::" << name;
#ifdef ACTORS_USE_GCD
     const char* queueLabel = dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL);
     if(*queueLabel != 0) {
         s << " [from queue " << queueLabel;
     } else
#endif
     {
         s << " [from thread " << GetThreadName();
     }

    if(after != 0) {
        s << " after " << after << " secs";
    }

    s << "]";

    {
        lock_guard<mutex> lock(_mutex);
        _enqueueCalls.emplace_back(ChannelManifestEntry { elapsed, s.str() });
        while(_enqueueCalls.size() > _limit) {
            _enqueueCalls.pop_front();
            _truncatedEnqueue++;
        }
    }
}

void ChannelManifest::addExecution(const Actor* actor, const char* name) {
    auto now = chrono::system_clock::now();
    auto elapsed = chrono::duration_cast<chrono::microseconds>(now - _start);
    stringstream s;
    s << actor->loggingName() << "::" << name;
#ifdef ACTORS_USE_GCD
     const char* queueLabel = dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL);
     if(*queueLabel != 0) {
         s << " [on queue " << queueLabel << "]";
     } else
#endif
     {
         s << " [on thread " << GetThreadName() << "]";
     }

    {
        lock_guard<mutex> lock(_mutex);
        _executions.emplace_back(ChannelManifestEntry { elapsed, s.str() });
        while(_executions.size() > _limit) {
            _executions.pop_front();
            _truncatedExecution++;
        }
    }
}

void ChannelManifest::dump(ostream& out) {
    lock_guard<mutex> lock(_mutex);
    out << "List of enqueue calls:" << endl;
    if(_truncatedEnqueue > 0) {
        out << "\t..." << _truncatedEnqueue << " truncated frames...";
    }

    for(const auto& entry : _enqueueCalls) {
        out << "\t[" << entry.elapsed.count() / 1000.0 << " ms] " << entry.description << endl;
    }

    out << "Resulting execution calls:" << endl;
    if(_truncatedExecution > 0) {
        out << "\t..." << _truncatedExecution << " truncated frames...";
    }
    for(const auto& entry : _executions) {
        out << "\t[" << entry.elapsed.count() / 1000.0 << " ms] " << entry.description << endl;
    }
}

#endif