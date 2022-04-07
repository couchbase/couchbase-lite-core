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

    virtual C4ConnectedClientParameters params() {
        return {};
    }

    void start() {
        std::unique_lock<std::mutex> lock(_mutex);
        Assert(!_serverRunning && !_clientRunning);

        auto serverOpts = make_retained<repl::Options>(kC4Passive,kC4Passive);
        serverOpts->setProperty(kC4ReplicatorOptionAllowConnectedClient, true);
        serverOpts->setProperty(kC4ReplicatorOptionNoIncomingConflicts, true);

        c4::ref<C4Database> serverDB = c4db_openAgain(db, ERROR_INFO());
        REQUIRE(serverDB);
        _server = new repl::Replicator(serverDB,
                                       new LoopbackWebSocket(alloc_slice("ws://srv/"),
                                                             Role::Server, {}),
                                       *this, serverOpts);
        
        _client = new client::ConnectedClient(new LoopbackWebSocket(alloc_slice("ws://cli/"),
                                                                    Role::Client, {}),
                                              *this,
                                              params());

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


    ~ConnectedClientLoopbackTest() {
        stop();
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
                             C4ReplicatorActivityLevel level) override {
        Log("+++ Client status changed: %d", int(level));
        if (level == kC4Stopped) {
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


#pragma mark - TESTS:


TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getRev", "[ConnectedClient]") {
    importJSONLines(sFixturesDir + "names_100.json");
    start();

    Log("++++ Calling ConnectedClient::getDoc()...");
    auto asyncResult1 = _client->getDoc("0000001", nullslice, nullslice);
    auto asyncResult99 = _client->getDoc("0000099", nullslice, nullslice);

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


TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getRev Conditional Match", "[ConnectedClient]") {
    importJSONLines(sFixturesDir + "names_100.json");
    start();

    auto match = _client->getDoc("0000002", nullslice,
                                 "1-1fdf9d4bdae09f6651938d9ec1d47177280f5a77");
    CHECK(waitForErrorResponse(match) == C4Error{WebSocketDomain, 304});
}


TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getRev Conditional No Match", "[ConnectedClient]") {
    importJSONLines(sFixturesDir + "names_100.json");
    start();

    auto match = _client->getDoc("0000002", nullslice,
                                 "1-beefbeefbeefbeefbeefbeefbeefbeefbeefbeef");
    auto rev = waitForResponse(match);
    CHECK(rev.docID == "0000002");
    CHECK(rev.revID == "1-1fdf9d4bdae09f6651938d9ec1d47177280f5a77");
    CHECK(rev.deleted == false);
    auto doc = Doc(rev.body);
    CHECK(doc.asDict()["birthday"].asString() == "1989-04-29");
}


TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getRev NotFound", "[ConnectedClient]") {
    start();
    auto asyncResultX = _client->getDoc("bogus", nullslice, nullslice);
    CHECK(waitForErrorResponse(asyncResultX) == C4Error{LiteCoreDomain, kC4ErrorNotFound});
}


TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getBlob", "[ConnectedClient]") {
    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
    }
    start();

    auto asyncResult1 = _client->getDoc("att1", nullslice, nullslice);
    auto rev = waitForResponse(asyncResult1);
    CHECK(rev.docID == "att1");
    auto doc = Doc(rev.body);
    auto digest = C4Blob::keyFromDigestProperty(doc.asDict()["attached"].asArray()[0].asDict());
    REQUIRE(digest);
    CHECK(*digest == blobKeys[0]);

    auto asyncBlob1 = _client->getBlob(blobKeys[0], true);
    auto asyncBlob2 = _client->getBlob(blobKeys[1], false);
    auto asyncBadBlob = _client->getBlob(C4BlobKey{}, false);

    alloc_slice blob1 = waitForResponse(asyncBlob1);
    CHECK(blob1 == slice(attachments[0]));

    alloc_slice blob2 = waitForResponse(asyncBlob2);
    CHECK(blob2 == slice(attachments[1]));

    CHECK(waitForErrorResponse(asyncBadBlob) == C4Error{LiteCoreDomain, kC4ErrorNotFound});
}


TEST_CASE_METHOD(ConnectedClientLoopbackTest, "putRev", "[ConnectedClient]") {
    importJSONLines(sFixturesDir + "names_100.json");
    start();

    Encoder enc;
    enc.beginDict();
    enc["connected"] = "client";
    enc.endDict();
    auto docBody = enc.finish();

    auto rq1 = _client->putDoc("0000001", nullslice,
                               "2-2222",
                               "1-4cbe54d79c405e368613186b0bc7ac9ee4a50fbb",
                               C4RevisionFlags{},
                               docBody);
    auto rq2 = _client->putDoc("frob", nullslice,
                               "1-1111",
                               nullslice,
                               C4RevisionFlags{},
                               docBody);
    rq1.blockUntilReady();
    REQUIRE(rq1.error() == C4Error());
    c4::ref<C4Document> doc1 = c4db_getDoc(db, "0000001"_sl, true, kDocGetCurrentRev, ERROR_INFO());
    REQUIRE(doc1);
    CHECK(doc1->revID == "2-2222"_sl);

    rq2.blockUntilReady();
    REQUIRE(rq2.error() == C4Error());
    c4::ref<C4Document> doc2 = c4db_getDoc(db, "frob"_sl, true, kDocGetCurrentRev, ERROR_INFO());
    REQUIRE(doc2);
    CHECK(doc2->revID == "1-1111"_sl);
}


TEST_CASE_METHOD(ConnectedClientLoopbackTest, "putDoc Failure", "[ConnectedClient]") {
    importJSONLines(sFixturesDir + "names_100.json");
    start();

    Encoder enc;
    enc.beginDict();
    enc["connected"] = "client";
    enc.endDict();
    auto docBody = enc.finish();

    auto rq1 = _client->putDoc("0000001", nullslice,
                               "2-2222",
                               "1-d00d",
                               C4RevisionFlags{},
                               docBody);
    rq1.blockUntilReady();
    REQUIRE(rq1.error() == C4Error{LiteCoreDomain, kC4ErrorConflict});
}


TEST_CASE_METHOD(ConnectedClientLoopbackTest, "observeCollection", "[ConnectedClient]") {
    {
        // Start with a single doc that should not be sent to the observer
        TransactionHelper t(db);
        createFleeceRev(db, "doc1"_sl, "1-1111"_sl, R"({"name":"Puddin' Tane"})"_sl);
    }
    start();

    mutex m;
    condition_variable cond;
    vector<C4CollectionObserver::Change> allChanges;

    _client->observeCollection(nullslice, [&](vector<C4CollectionObserver::Change> const& changes) {
        // Observer callback:
        unique_lock<mutex> lock(m);
        Log("+++ Observer got %zu changes!", changes.size());
        allChanges.insert(allChanges.end(), changes.begin(), changes.end());
        cond.notify_one();
    }).then([&](C4Error error) {
        // Async callback when the observer has started:
        unique_lock<mutex> lock(m);
        REQUIRE(error == C4Error{});
        Log("+++ Importing docs...");
        importJSONLines(sFixturesDir + "names_100.json");
    });

    Log("+++ Waiting for 100 changes to arrive...");
    unique_lock<mutex> lock(m);
    cond.wait(lock, [&]{return allChanges.size() >= 100;});

    Log("+++ Checking the changes");
    REQUIRE(allChanges.size() == 100);
    C4SequenceNumber expectedSeq = 2;
    for (auto &change : allChanges) {
        CHECK(change.docID.size == 7);
        CHECK(change.flags == 0);
        CHECK(change.sequence == expectedSeq++);
    }
}


#pragma mark - ENCRYPTION:

#if 0 //TEMP
#ifdef COUCHBASE_ENTERPRISE

class ConnectedClientEncryptedLoopbackTest : public ConnectedClientLoopbackTest {

    C4ConnectedClientParameters params() override{
        auto p = ConnectedClientLoopbackTest::params();
        p.propertyEncryptor = &encryptor;
        p.propertyDecryptor = &decryptor;
        p.callbackContext = &_encryptorContext;
    }

    static alloc_slice unbreakableEncryption(slice cleartext, int8_t delta) {
        alloc_slice ciphertext(cleartext);
        for (size_t i = 0; i < ciphertext.size; ++i)
            (uint8_t&)ciphertext[i] += delta;        // "I've got patent pending on that!" --Wallace
        return ciphertext;
    }

    struct TestEncryptorContext {
        slice docID;
        slice keyPath;
        bool called;
    } _encryptorContext;

    static C4SliceResult encryptor(void* rawCtx,
                                   C4String documentID,
                                   FLDict properties,
                                   C4String keyPath,
                                   C4Slice input,
                                   C4StringResult* outAlgorithm,
                                   C4StringResult* outKeyID,
                                   C4Error* outError)
    {
        auto context = (TestEncryptorContext*)rawCtx;
        context->called = true;
        CHECK(documentID == context->docID);
        CHECK(keyPath == context->keyPath);
        return C4SliceResult(unbreakableEncryption(input, 1));
    }

    static C4SliceResult decryptor(void* rawCtx,
                                   C4String documentID,
                                   FLDict properties,
                                   C4String keyPath,
                                   C4Slice input,
                                   C4String algorithm,
                                   C4String keyID,
                                   C4Error* outError)
    {
        auto context = (TestEncryptorContext*)rawCtx;
        context->called = true;
        CHECK(documentID == context->docID);
        CHECK(keyPath == context->keyPath);
        return C4SliceResult(unbreakableEncryption(input, -1));
    }

};


TEST_CASE_METHOD(ConnectedClientEncryptedLoopbackTest, "getRev encrypted", "[ConnectedClient]") {
    createFleeceRev(db, "seekrit"_sl, "1-1111"_sl,
        R"({"encrypted$SSN":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"IzIzNC41Ni43ODk6Iw=="}})"_sl);
    start();

    Log("++++ Calling ConnectedClient::getDoc()...");
    auto asyncResult1 = _client->getDoc("seekrit", nullslice, nullslice);
    auto rev = waitForResponse(asyncResult1);
    Doc doc(rev.body);
    CHECK(doc.asDict()["SSN"].toJSONString() == R"({"SSN":{"@type":"encryptable","value":"123-45-6789"}})");
}

#endif
#endif //TEMP
