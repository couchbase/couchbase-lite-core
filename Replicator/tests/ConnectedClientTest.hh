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
class ConnectedClientLoopbackTest : public C4Test,
                                    public repl::Replicator::Delegate,
                                    public client::ConnectedClient::Delegate
{
public:

    ConnectedClientLoopbackTest() {
        _serverOptions = make_retained<repl::Options>(kC4Passive,kC4Passive);
        _serverOptions->setProperty(kC4ReplicatorOptionAllowConnectedClient, true);
        _serverOptions->setProperty(kC4ReplicatorOptionNoIncomingConflicts, true);
    }

    ~ConnectedClientLoopbackTest() {
        stop();
    }

    void start() {
        std::unique_lock<std::mutex> lock(_mutex);
        Assert(!_serverRunning && !_clientRunning);

        c4::ref<C4Database> serverDB = c4db_openAgain(db, ERROR_INFO());
        REQUIRE(serverDB);
        _server = new repl::Replicator(serverDB,
                                       new LoopbackWebSocket(alloc_slice("ws://srv/"),
                                                             Role::Server, {}),
                                       *this, _serverOptions);

        _client = new client::ConnectedClient(new LoopbackWebSocket(alloc_slice("ws://cli/"),
                                                                    Role::Client, {}),
                                              *this,
                                              _params);

        Headers headers;
        headers.add("Set-Cookie"_sl, "flavor=chocolate-chip"_sl);
        LoopbackWebSocket::bind(_server->webSocket(), _client->webSocket(), headers);

        _clientRunning = _serverRunning = true;
        _server->start();
        _client->start();
    }


    void stop() {
        std::unique_lock<std::mutex> lock(_mutex);
        if (_server) {
            _server->stop();
            _server = nullptr;
        }
        if (_client) {
            _client->stop();
            _client = nullptr;
        }

        Log("+++ Waiting for client & replicator to stop...");
        _cond.wait(lock, [&]{return !_clientRunning && !_serverRunning;});
    }


    template <class T>
    auto waitForResponse(actor::Async<T> &asyncResult) {
        asyncResult.blockUntilReady();

        Log("++++ Async response available!");
        if (auto err = asyncResult.error())
            FAIL("Response returned an error " << err);
        return asyncResult.result().value();
    }


    template <class T>
    C4Error waitForErrorResponse(actor::Async<T> &asyncResult) {
        asyncResult.blockUntilReady();

        Log("++++ Async response available!");
        auto err = asyncResult.error();
        if (!err)
            FAIL("Response did not return an error");
        return err;
    }


    //---- ConnectedClient delegate:

    alloc_slice getBlobContents(const C4BlobKey &blobKey, C4Error *error) override {
        string digestString = blobKey.digestString();
        if (auto i = _blobs.find(digestString); i != _blobs.end()) {
            alloc_slice blob = i->second;
            _blobs.erase(i);    // remove blob after it's requested
            return blob;
        } else {
            WarnError("getBlobContents called on unknown blob %s", digestString.c_str());
            *error = C4Error::make(LiteCoreDomain, kC4ErrorNotFound);
            return nullslice;
        }
    }


    void clientGotHTTPResponse(client::ConnectedClient* NONNULL,
                               int status,
                               const websocket::Headers &headers) override
    {
        Log("+++ Client got HTTP response");
    }

    void clientGotTLSCertificate(client::ConnectedClient* NONNULL,
                                 slice certData) override
    {
        Log("+++ Client got TLS certificate");
    }

    void clientStatusChanged(client::ConnectedClient* NONNULL,
                             client::ConnectedClient::Status const& status) override
    {
        Log("+++ Client status changed: %d", int(status.level));
        if (status.level == kC4Stopped) {
            std::unique_lock<std::mutex> lock(_mutex);
            _clientRunning = false;
            if (!_clientRunning && !_serverRunning)
                _cond.notify_all();
        }
    }

    void clientConnectionClosed(client::ConnectedClient* NONNULL,
                                const CloseStatus &close) override {
        Log("+++ Client connection closed: reason=%d, code=%d, message=%.*s",
              int(close.reason), close.code, FMTSLICE(close.message));
    }


    //---- Replicator delegate:

    void replicatorGotHTTPResponse(repl::Replicator* NONNULL,
                                   int status,
                                   const websocket::Headers &headers) override { }
    void replicatorGotTLSCertificate(slice certData) override { }
    void replicatorStatusChanged(repl::Replicator* NONNULL,
                                 const repl::Replicator::Status &status) override {
        if (status.level == kC4Stopped) {
            std::unique_lock<std::mutex> lock(_mutex);
            _serverRunning = false;
            if (!_clientRunning && !_serverRunning)
                _cond.notify_all();
        }
    }
    void replicatorConnectionClosed(repl::Replicator* NONNULL,
                                    const CloseStatus&) override { }
    void replicatorDocumentsEnded(repl::Replicator* NONNULL,
                                  const repl::Replicator::DocumentsEnded&) override { }
    void replicatorBlobProgress(repl::Replicator* NONNULL,
                                const repl::Replicator::BlobProgress&) override { }


    C4ConnectedClientParameters         _params {};
    Retained<repl::Replicator>          _server;
    Retained<repl::Options>             _serverOptions;
    Retained<client::ConnectedClient>   _client;
    mutex                               _mutex;
    condition_variable                  _cond;
    unordered_map<string,alloc_slice>   _blobs;
    bool                                _clientRunning = false;
    bool                                _serverRunning = false;
};
