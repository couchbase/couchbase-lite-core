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
#include "Replicator.hh"
#include "Address.hh"
#include "c4Socket+Internal.hh"
#include "LoopbackProvider.hh"
#include "Error.hh"

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::net;
using namespace litecore::repl;
using namespace litecore::websocket;


static const char *kReplicatorProtocolName = "+CBMobile_2";


struct C4Replicator : public RefCounted, Replicator::Delegate {


    static Replicator::Options replOpts(const C4ReplicatorParameters &params) {
        Replicator::Options opts(params.push, params.pull, params.optionsDictFleece);
        opts.pushFilter = params.pushFilter;
        opts.pullValidator = params.validationFunc;
        opts.callbackContext = params.callbackContext;
        return opts;
    }


    static alloc_slice socketOpts(C4Database *db,
                                  const C4Address &serverAddress,
                                  const C4ReplicatorParameters &params)
    {
        Replicator::Options opts(kC4Disabled, kC4Disabled, params.optionsDictFleece);
        opts.setProperty(slice(kC4SocketOptionWSProtocols),
                         (string(Connection::kWSProtocolName) + kReplicatorProtocolName).c_str());
        return opts.properties.data();
    }

    // Appends the db name and "/_blipsync" to the Address's path, then returns the resulting URL.
    static alloc_slice effectiveURL(C4Address address, slice remoteDatabaseName) {
        slice path = address.path;
        string newPath = string(path);
        if (!path.hasSuffix("/"_sl))
            newPath += "/";
        newPath += string(remoteDatabaseName) + "/_blipsync";
        address.path = slice(newPath);
        return Address::toURL(address);
    }


    // Constructor for replication with remote database
    C4Replicator(C4Database* db,
                 const C4Address &serverAddress,
                 C4String remoteDatabaseName,
                 const C4ReplicatorParameters &params)
    :C4Replicator(new Replicator(db,
                                 CreateWebSocket(effectiveURL(serverAddress, remoteDatabaseName),
                                                 Role::Client,
                                                 socketOpts(db, serverAddress, params),
                                                 db,
                                                 params.socketFactory),
                                 *this,
                                 replOpts(params)),
                  nullptr,
                  params)
    { }

    // Constructor for replication with local database
    C4Replicator(C4Database* db,
                 C4Database* otherDB,
                 const C4ReplicatorParameters &params)
    :C4Replicator(new Replicator(db,
                                 new LoopbackWebSocket(Address(db), Role::Client),
                                 *this,
                                 replOpts(params).setNoDeltas()),
                  new Replicator(otherDB,
                                 new LoopbackWebSocket(Address(otherDB), Role::Server),
                                 *this,
                                 Replicator::Options(kC4Passive, kC4Passive).setNoIncomingConflicts()
                                                                            .setNoDeltas()),
                  params)
    {
        LoopbackWebSocket::bind(_replicator->webSocket(), _otherReplicator->webSocket());
        _otherLevel = _otherReplicator->status().level;
    }

    // Constructor for already-open socket
    C4Replicator(C4Database* db,
                 C4Socket *openSocket,
                 const C4ReplicatorParameters &params)
    :C4Replicator(new Replicator(db, WebSocketFrom(openSocket), *this, replOpts(params)),
                  nullptr,
                  params)
    { }

    void start(bool synchronous =false) {
        DebugAssert(!_selfRetain);
        if (_otherReplicator)
            _otherReplicator->start(synchronous);
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

private:
    // base constructor
    C4Replicator(Replicator *replicator,
                 Replicator *otherReplicator,
                 const C4ReplicatorParameters &params)
    :_replicator(replicator)
    ,_otherReplicator(otherReplicator)
    ,_params(params)
    ,_status(_replicator->status())
    { }

    virtual ~C4Replicator() =default;


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
        bool done;
        {
            lock_guard<mutex> lock(_mutex);
            if (repl == _replicator)
                _status = newStatus;
            else if (repl == _otherReplicator)
                _otherLevel = newStatus.level;
            done = (_status.level == kC4Stopped && _otherLevel == kC4Stopped);
        }

        if (repl == _replicator)
            notifyStateChanged();
        if (done)
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
    Retained<Replicator> const _replicator;
    Retained<Replicator> const _otherReplicator;
    C4ReplicatorParameters _params;
    alloc_slice _responseHeaders;
    C4ReplicatorStatus _status;
    C4ReplicatorActivityLevel _otherLevel {kC4Stopped};
    Retained<C4Replicator> _selfRetain;
};
