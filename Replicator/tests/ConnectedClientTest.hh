//
// ConnectedClientTest.hh
//
// Copyright © 2022 Couchbase. All rights reserved.
//

#pragma once
#include "c4Test.hh"
#include "ConnectedClient.hh"
#include "Replicator.hh"
#include "LoopbackProvider.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"
#include <unordered_map>


using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::websocket;

/// Test class for Connected Client unit tests. Runs a LiteCore replicator in passive mode.
class ConnectedClientLoopbackTest
    : public C4Test
    , public repl::Replicator::Delegate
    , public client::ConnectedClient::Delegate {
  public:
    ConnectedClientLoopbackTest()
        : kRev1ID(isRevTrees() ? "1-1111" : "123@ZegpoldZegpoldZegpoldA")
        , kRev2ID(isRevTrees() ? "2-2222" : "99@ZegpoldZegpoldZegpoldA") {
        _serverOptions = make_retained<repl::Options>(kC4Passive, kC4Passive);
        _serverOptions->setProperty(kC4ReplicatorOptionAllowConnectedClient, true);
        _serverOptions->setProperty(kC4ReplicatorOptionNoIncomingConflicts, true);

        repl::Options::CollectionOptions coll(kC4DefaultCollectionSpec);
        coll.push = coll.pull = kC4Passive;
        _serverOptions->collectionOpts.push_back(std::move(coll));
    }

    ~ConnectedClientLoopbackTest() { stop(); }

    void start() {
        {
            unique_lock lock(_mutex);
            Assert(!_serverRunning && !_clientRunning);

            c4::ref<C4Database> serverDB = c4db_openAgain(db, ERROR_INFO());
            REQUIRE(serverDB);
            _server = new repl::Replicator(serverDB, new LoopbackWebSocket(alloc_slice("ws://srv/"), Role::Server, {}),
                                           *this, _serverOptions);
            auto clientOptions = make_retained<repl::Options>(kC4Passive, kC4Passive);
            _client = new client::ConnectedClient(db, new LoopbackWebSocket(alloc_slice("ws://cli/"), Role::Client, {}),
                                                  *this, _params, clientOptions);

            Headers headers;
            headers.add("Set-Cookie"_sl, "flavor=chocolate-chip"_sl);
            LoopbackWebSocket::bind(_server->webSocket(), _client->webSocket(), headers);
        }

        _server->start();
        _client->start();

        unique_lock lock(_mutex);
        _cond.wait(lock, [this] { return _clientRunning && _serverRunning; });
    }

    void stop() {
        unique_lock lock(_mutex);
        if ( _server ) {
            _server->stop();
            _server = nullptr;
        }
        if ( _client ) {
            _client->stop();
            _client = nullptr;
        }

        Log("+++ Waiting for client & replicator to stop...");
        _cond.wait(lock, [&] { return !_clientRunning && !_serverRunning; });
    }

    //---- ConnectedClient delegate:

    alloc_slice getBlobContents(const C4BlobKey& blobKey, C4Error* error) override {
        string digestString = blobKey.digestString();
        if ( auto i = _blobs.find(digestString); i != _blobs.end() ) {
            alloc_slice blob = i->second;
            _blobs.erase(i);  // remove blob after it's requested
            return blob;
        } else {
            WarnError("getBlobContents called on unknown blob %s", digestString.c_str());
            *error = C4Error::make(LiteCoreDomain, kC4ErrorNotFound);
            return nullslice;
        }
    }

    void clientStatusChanged(client::ConnectedClient* client, client::ConnectedClient::Status const& status) override {
        Log("+++ Client status changed: %d", int(status.level));

        unique_lock lock(_mutex);
        bool        running = (status.level == kC4Idle || status.level == kC4Busy);
        if ( running != _clientRunning ) {
            if ( running ) CHECK(client->responseHeaders() != nullslice);
            _clientRunning = running;
            _cond.notify_all();
        }
    }

    void clientConnectionClosed(client::ConnectedClient*, const CloseStatus& close) override {
        Log("+++ Client connection closed: reason=%d, code=%d, message=%.*s", int(close.reason), close.code,
            FMTSLICE(close.message));
    }

    //---- Replicator delegate:

    void replicatorGotHTTPResponse(repl::Replicator*, int status, const websocket::Headers& headers) override {}

    void replicatorGotTLSCertificate(slice certData) override {}

    void replicatorStatusChanged(repl::Replicator*, const repl::Replicator::Status& status) override {
        Log("+++ Server status changed: %d", int(status.level));
        unique_lock lock(_mutex);
        bool        running = (status.level == kC4Idle || status.level == kC4Busy);
        if ( running != _serverRunning ) {
            _serverRunning = running;
            _cond.notify_all();
        }
    }

    void replicatorConnectionClosed(repl::Replicator*, const CloseStatus&) override {}

    void replicatorDocumentsEnded(repl::Replicator*, const repl::Replicator::DocumentsEnded&) override {}

    void replicatorBlobProgress(repl::Replicator*, const repl::Replicator::BlobProgress&) override {}

    //---- Utilities:

    alloc_slice actualRevID(slice docID) const {
        c4::ref<C4Document> doc = c4db_getDoc(db, docID, true, kDocGetMetadata, ERROR_INFO());
        REQUIRE(doc);
        return alloc_slice(c4doc_getSelectedRevIDGlobalForm(doc));
    }

    /// Returns a function that when called will copy its Result to `result` and notify.
    template <typename T>
    function<void(Result<T>)> expect(Result<T>& result) {
        ++_waitCount;
        return [&](Result<T> gotResponse) {
            result = std::move(gotResponse);
            notify();
        };
    }

    /// Each call decrements the count.
    void notify() {
        unique_lock lock(_mutex);
        Assert(_waitCount > 0);
        if ( --_waitCount == 0 ) _cond.notify_one();
    }

    /// Waits until the count reaches zero.
    void wait() {
        unique_lock lock(_mutex);
        Assert(_waitCount > 0);
        _cond.wait(lock, [&] { return _waitCount == 0; });
    }

    slice kRev1ID, kRev2ID;

    C4ConnectedClientParameters        _params{};
    Retained<repl::Replicator>         _server;
    Retained<repl::Options>            _serverOptions;
    Retained<client::ConnectedClient>  _client;
    mutex                              _mutex;
    condition_variable                 _cond;
    unordered_map<string, alloc_slice> _blobs;
    unsigned                           _waitCount     = 0;
    bool                               _clientRunning = false;
    bool                               _serverRunning = false;
};
