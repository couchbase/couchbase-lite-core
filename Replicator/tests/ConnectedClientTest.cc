//
// ConnectedClientTest.cc
//
// Copyright © 2022 Couchbase. All rights reserved.
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

#include "c4Test.hh"
#include "ConnectedClient.hh"
#include "Replicator.hh"
#include "LoopbackProvider.hh"
#include "fleece/Fleece.hh"


using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::websocket;


class ConnectedClientLoopbackTest : public C4Test,
                                    public repl::Replicator::Delegate,
                                    public client::ConnectedClient::Delegate
{
public:

    void start() {
        std::unique_lock<std::mutex> lock(_mutex);
        Assert(!_serverRunning && !_clientRunning);

        auto serverOpts = make_retained<repl::Options>(kC4Passive,kC4Passive);
        _server = new repl::Replicator(db,
                                       new LoopbackWebSocket(alloc_slice("ws://srv/"),
                                                             Role::Server, {}),
                                       *this, serverOpts);
        AllocedDict clientOpts;
        _client = new client::ConnectedClient(new LoopbackWebSocket(alloc_slice("ws://cli/"),
                                                                    Role::Client, {}),
                                              *this,
                                              clientOpts);

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

        Log("Waiting for client & replicator to stop...");
        _cond.wait(lock, [&]{return !_clientRunning && !_serverRunning;});
    }


    client::DocResponse waitForResponse(actor::Async<client::DocResponseOrError> &asyncResult) {
        asyncResult.blockUntilReady();

        C4Log("++++ Async response available!");
        auto &result = asyncResult.result();
        if (auto err = std::get_if<C4Error>(&result))
            FAIL("Response returned an error " << *err);
        auto rev = std::get_if<client::DocResponse>(&result);
        REQUIRE(rev);
        return *rev;
    }


    C4Error waitForErrorResponse(actor::Async<client::DocResponseOrError> &asyncResult) {
        asyncResult.blockUntilReady();

        C4Log("++++ Async response available!");
        auto &result = asyncResult.result();
        auto err = std::get_if<C4Error>(&result);
        if (!err)
            FAIL("Response unexpectedly didn't fail");
        return *err;
    }


    ~ConnectedClientLoopbackTest() {
        stop();
    }


    void clientGotHTTPResponse(client::ConnectedClient* NONNULL,
                               int status,
                               const websocket::Headers &headers) override
    {
        C4Log("Client got HTTP response");
    }
    void clientGotTLSCertificate(client::ConnectedClient* NONNULL,
                                 slice certData) override
    {
        C4Log("Client got TLS certificate");
    }
    void clientStatusChanged(client::ConnectedClient* NONNULL,
                             C4ReplicatorActivityLevel level) override {
        C4Log("Client status changed: %d", int(level));
        if (level == kC4Stopped) {
            std::unique_lock<std::mutex> lock(_mutex);
            _clientRunning = false;
            if (!_clientRunning && !_serverRunning)
                _cond.notify_all();
        }
    }
    void clientConnectionClosed(client::ConnectedClient* NONNULL,
                                const CloseStatus &close) override {
        C4Log("Client connection closed: reason=%d, code=%d, message=%.*s",
              int(close.reason), close.code, FMTSLICE(close.message));
    }


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


    Retained<repl::Replicator> _server;
    Retained<client::ConnectedClient> _client;
    bool _clientRunning = false, _serverRunning = false;
    mutex _mutex;
    condition_variable _cond;
};


TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getRev", "[ConnectedClient]") {
    importJSONLines(sFixturesDir + "names_100.json");
    start();

    C4Log("++++ Calling ConnectedClient::getDoc()...");
    auto asyncResult1 = _client->getDoc(alloc_slice("0000001"), nullslice, nullslice);
    auto asyncResult99 = _client->getDoc(alloc_slice("0000099"), nullslice, nullslice);

    auto rev = waitForResponse(asyncResult1);
    CHECK(rev.docID == "0000001");
    CHECK(rev.revID == "1-4cbe54d79c405e368613186b0bc7ac9ee4a50fbb");
    CHECK(rev.deleted == false);
    Doc doc(rev.body);
    CHECK(doc.asDict()["birthday"].asString() == "1983-09-18");

    rev = waitForResponse(asyncResult99);
    CHECK(rev.docID == "0000099");
    CHECK(rev.revID == "1-94baf6e4e4a1442aa6d8e9aab87955b8b7f4817a");
    CHECK(rev.deleted == false);
    doc = Doc(rev.body);
    CHECK(doc.asDict()["birthday"].asString() == "1958-12-20");
}


TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getRev NotFound", "[ConnectedClient]") {
    start();
    auto asyncResultX = _client->getDoc(alloc_slice("bogus"), nullslice, nullslice);
    CHECK(waitForErrorResponse(asyncResultX) == C4Error{LiteCoreDomain, kC4ErrorNotFound});
    C4Log("++++ Stopping...");
    stop();
}
