//
// ChannelManifest.cc
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "ChannelManifest.hh"

#if ACTORS_USE_MANIFESTS

#    include "ThreadUtil.hh"
#    include "Actor.hh"
#    include <sstream>
#    include <string>
#    include <chrono>

using namespace std;
using namespace litecore;
using namespace litecore::actor;

void ChannelManifest::addEnqueueCall(const Actor* actor, const char* name, double after) {
    auto         now     = chrono::system_clock::now();
    auto         elapsed = chrono::duration_cast<chrono::microseconds>(now - _start);
    stringstream s;
    s << actor->loggingName() << "::" << name;
#    ifdef ACTORS_USE_GCD
    const char* queueLabel = dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL);
    if ( *queueLabel != 0 ) {
        s << " [from queue " << queueLabel;
    } else
#    endif
    {
        s << " [from thread " << GetThreadName();
    }

    if ( after != 0 ) { s << " after " << after << " secs"; }

    s << "]";

    {
        lock_guard<mutex> lock(_mutex);
        _enqueueCalls.emplace_back(ChannelManifestEntry{elapsed, s.str()});
        while ( _enqueueCalls.size() > _limit ) {
            _enqueueCalls.pop_front();
            _truncatedEnqueue++;
        }
    }
}

void ChannelManifest::addExecution(const Actor* actor, const char* name) {
    auto         now     = chrono::system_clock::now();
    auto         elapsed = chrono::duration_cast<chrono::microseconds>(now - _start);
    stringstream s;
    s << actor->loggingName() << "::" << name;
#    ifdef ACTORS_USE_GCD
    const char* queueLabel = dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL);
    if ( *queueLabel != 0 ) {
        s << " [on queue " << queueLabel << "]";
    } else
#    endif
    {
        s << " [on thread " << GetThreadName() << "]";
    }

    {
        lock_guard<mutex> lock(_mutex);
        _executions.emplace_back(ChannelManifestEntry{elapsed, s.str()});
        while ( _executions.size() > _limit ) {
            _executions.pop_front();
            _truncatedExecution++;
        }
    }
}

void ChannelManifest::dump(ostream& out) {
    lock_guard<mutex> lock(_mutex);
    out << "List of enqueue calls:" << endl;
    if ( _truncatedEnqueue > 0 ) { out << "\t..." << _truncatedEnqueue << " truncated frames..."; }

    for ( const auto& entry : _enqueueCalls ) {
        out << "\t[" << entry.elapsed.count() / 1000.0 << " ms] " << entry.description << endl;
    }

    out << "Resulting execution calls:" << endl;
    if ( _truncatedExecution > 0 ) { out << "\t..." << _truncatedExecution << " truncated frames..."; }
    for ( const auto& entry : _executions ) {
        out << "\t[" << entry.elapsed.count() / 1000.0 << " ms] " << entry.description << endl;
    }
}

#endif
