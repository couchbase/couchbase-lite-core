//
// c4Replicator.hh
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

#pragma once

#include "fleece/Fleece.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Replicator.h"
#include "Database.hh"
#include "Replicator.hh"
#include "Headers.hh"
#include "Error.hh"

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::repl;


static const char *kReplicatorProtocolName = "+CBMobile_2";


/** Glue between C4 API and internal LiteCore replicator. Abstract class. */
struct C4Replicator : public RefCounted, Replicator::Delegate {

    virtual void start(bool synchronous =false) {
        DebugAssert(_replicator);
        DebugAssert(!_selfRetain);
        _status = _replicator->status();
        _selfRetain = this; // keep myself alive till Replicator stops
        _replicator->start(synchronous);
    }

    alloc_slice responseHeaders() {
        lock_guard<mutex> lock(_mutex);
        return _responseHeaders;
    }

    C4ReplicatorStatus status() {
        lock_guard<mutex> lock(_mutex);
        return _status;
    }

    void stop() {
        _replicator->stop();
    }

    void detach() {
        lock_guard<mutex> lock(_mutex);
        _params.onStatusChanged = nullptr;
        _params.onDocumentsEnded = nullptr;
    }

protected:
    // base constructor
    C4Replicator(C4Database* db,
                 const C4ReplicatorParameters &params)
    :_database(db)
    ,_params(params)
    { }


    Replicator::Options options() {
        Replicator::Options opts(_params.push, _params.pull, _params.optionsDictFleece);
        opts.pushFilter = _params.pushFilter;
        opts.pullValidator = _params.validationFunc;
        opts.callbackContext = _params.callbackContext;
        return opts;
    }


    // ReplicatorDelegate API:

    virtual void replicatorGotHTTPResponse(Replicator *repl, int status,
                                           const websocket::Headers &headers) override
    {
        lock_guard<mutex> lock(_mutex);
        if (repl == _replicator) {
            Assert(!_responseHeaders);
            _responseHeaders = headers.encode();
        }
    }

    virtual void replicatorStatusChanged(Replicator *repl,
                                         const Replicator::Status &newStatus) override
    {
        {
            lock_guard<mutex> lock(_mutex);
            if (repl != _replicator)
                return;
            _status = newStatus;
        }
        notifyStateChanged();

        if (newStatus.level == kC4Stopped)
            _selfRetain = nullptr; // balances retain in constructor
    }

    virtual void replicatorDocumentsEnded(Replicator *repl,
                          const std::vector<Retained<ReplicatedRev>>& revs) override
    {
        if (repl != _replicator)
            return;
        C4ReplicatorDocumentsEndedCallback onDocsEnded;
        {
            lock_guard<mutex> lock(_mutex);
            onDocsEnded = _params.onDocumentsEnded;
        }
        if (!onDocsEnded)
            return;

        auto nRevs = revs.size();
        vector<const C4DocumentEnded*> docsEnded;
        docsEnded.reserve(nRevs);
        for (int pushing = 0; pushing <= 1; ++pushing) {
            docsEnded.clear();
            for (auto rev : revs) {
                if ((rev->dir() == Dir::kPushing) == pushing)
                    docsEnded.push_back(rev->asDocumentEnded());
            }
            if (!docsEnded.empty())
                onDocsEnded(this, pushing, docsEnded.size(), docsEnded.data(), _params.callbackContext);
        }
    }

    virtual void replicatorBlobProgress(Replicator *repl,
                                        const Replicator::BlobProgress &p) override
    {
        if (repl != _replicator)
            return;
        C4ReplicatorBlobProgressCallback onBlob;
        {
            lock_guard<mutex> lock(_mutex);
            onBlob = _params.onBlobProgress;
        }
        if (onBlob)
            onBlob(this, (p.dir == Dir::kPushing),
                   {p.docID.buf, p.docID.size},
                   {p.docProperty.buf, p.docProperty.size},
                   p.key,
                   p.bytesCompleted, p.bytesTotal,
                   p.error,
                   _params.callbackContext);
    }

    void notifyStateChanged() {
        C4ReplicatorStatusChangedCallback onStatusChanged;
        {
            lock_guard<mutex> lock(_mutex);
            onStatusChanged = _params.onStatusChanged;
        }
        if (onStatusChanged)
            onStatusChanged(this, _status, _params.callbackContext);
    }

    mutex _mutex;
    Retained<C4Database> const  _database;
    C4ReplicatorParameters      _params;

    Retained<Replicator>        _replicator;
    alloc_slice                 _responseHeaders;
    C4ReplicatorStatus          _status {kC4Stopped};
    Retained<C4Replicator>      _selfRetain;            // Keeps me from being deleted
};
