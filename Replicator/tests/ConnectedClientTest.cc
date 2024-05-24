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

#include "ConnectedClientTest.hh"
#include "c4BlobStore.hh"
#include "fleece/Mutable.hh"


static constexpr C4Error placeholder{LiteCoreDomain, kC4ErrorUnexpectedError};


#pragma mark - GET:

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getRev", "[ConnectedClient]") {
    importJSONLines(sFixturesDir + "names_100.json");
    start();

    Result<client::DocResponse> rev1  = placeholder;
    Result<client::DocResponse> rev99 = placeholder;

    Log("++++ Calling ConnectedClient::getDoc()...");
    _client->getDoc(kC4DefaultCollectionSpec, "0000001", nullslice, true, this->expect(rev1));
    _client->getDoc(kC4DefaultCollectionSpec, "0000099", nullslice, true, this->expect(rev99));
    wait();

    REQUIRE(rev1.error() == kC4NoError);
    CHECK(rev1.value().docID == "0000001");
    CHECK(rev1.value().revID == actualRevID("0000001"));
    CHECK(rev1.value().deleted == false);
    Doc doc(rev1.value().body);
    CHECK(doc.asDict()["birthday"].asString() == "1983-09-18");

    REQUIRE(rev99.error() == kC4NoError);
    CHECK(rev99.value().docID == "0000099");
    CHECK(rev99.value().revID == actualRevID("0000099"));
    CHECK(rev99.value().deleted == false);
    doc = Doc(rev99.value().body);
    CHECK(doc.asDict()["birthday"].asString() == "1958-12-20");
}

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getRev Conditional Match", "[ConnectedClient]") {
    importJSONLines(sFixturesDir + "names_100.json");
    start();

    C4Error err = {};
    ++_waitCount;
    _client->getDoc(kC4DefaultCollectionSpec, "0000002", actualRevID("0000002"), true,
                    [&](Result<client::DocResponse> r) {
                        err = r.error();
                        notify();
                    });
    wait();
    CHECK(err == C4Error{WebSocketDomain, 304});
}

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getRev Conditional No Match", "[ConnectedClient]") {
    importJSONLines(sFixturesDir + "names_100.json");
    start();

    Result<client::DocResponse> rev = placeholder;
    _client->getDoc(kC4DefaultCollectionSpec, "0000002", "1-beefbeefbeefbeefbeefbeefbeefbeefbeefbeef", true,
                    this->expect(rev));
    wait();

    CHECK(rev.error() == kC4NoError);
    CHECK(rev.value().docID == "0000002");
    CHECK(rev.value().revID == actualRevID("0000002"));
    CHECK(rev.value().deleted == false);
    auto doc = Doc(rev.value().body);
    CHECK(doc.asDict()["birthday"].asString() == "1989-04-29");
}

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getRev NotFound", "[ConnectedClient]") {
    start();

    Result<client::DocResponse> rev = placeholder;
    _client->getDoc(kC4DefaultCollectionSpec, "bogus", nullslice, true, this->expect(rev));
    wait();
    CHECK(rev.error() == C4Error{LiteCoreDomain, kC4ErrorNotFound});
}

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getBlob", "[ConnectedClient]") {
    vector<string>    attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
    }
    start();

    Result<client::DocResponse> rev = placeholder;

    _client->getDoc(kC4DefaultCollectionSpec, "att1", nullslice, true, this->expect(rev));
    wait();

    REQUIRE(rev.error() == kC4NoError);
    CHECK(rev.value().docID == "att1");
    auto doc    = Doc(rev.value().body);
    auto digest = C4Blob::keyFromDigestProperty(doc.asDict()["attached"].asArray()[0].asDict());
    REQUIRE(digest);
    CHECK(*digest == blobKeys[0]);

    Result<alloc_slice> blob1 = placeholder, blob2 = placeholder, badBlob = placeholder;
    {
        _client->getBlob(kC4DefaultCollectionSpec, blobKeys[0], true, this->expect(blob1));
        _client->getBlob(kC4DefaultCollectionSpec, blobKeys[1], false, this->expect(blob2));
        _client->getBlob(kC4DefaultCollectionSpec, C4BlobKey{}, false, this->expect(badBlob));
        wait();
    }

    REQUIRE(blob1.error() == kC4NoError);
    CHECK(blob1.value() == slice(attachments[0]));
    REQUIRE(blob2.error() == kC4NoError);
    CHECK(blob2.value() == slice(attachments[1]));
    CHECK(badBlob.error() == C4Error{LiteCoreDomain, kC4ErrorNotFound});
}

#pragma mark - PUT:

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "putDoc", "[ConnectedClient]") {
    importJSONLines(sFixturesDir + "names_100.json");
    start();

    Encoder enc;
    enc.beginDict();
    enc["connected"] = "client";
    enc.endDict();
    auto docBody = enc.finish();

    Result<void> result;

    slice newRevID1 = isRevTrees() ? "2-2222" : "99@ZegpoldZegpoldZegpoldA";
    _client->putDoc(kC4DefaultCollectionSpec, "0000001", newRevID1, actualRevID("0000001"), C4RevisionFlags{}, docBody,
                    this->expect(result));
    wait();
    REQUIRE(result.error() == kC4NoError);

    slice newRevID2 = isRevTrees() ? "1-1111" : "123@ZegpoldZegpoldZegpoldA";
    _client->putDoc(kC4DefaultCollectionSpec, "frob", newRevID2, nullslice, C4RevisionFlags{}, docBody,
                    this->expect(result));
    wait();
    REQUIRE(result.error() == kC4NoError);

    c4::ref<C4Document> doc1 = c4db_getDoc(db, "0000001"_sl, true, kDocGetCurrentRev, ERROR_INFO());
    REQUIRE(doc1);
    CHECK(doc1->revID == newRevID1);

    c4::ref<C4Document> doc2 = c4db_getDoc(db, "frob"_sl, true, kDocGetCurrentRev, ERROR_INFO());
    REQUIRE(doc2);
    CHECK(doc2->revID == newRevID2);
}

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "putDoc Failure", "[ConnectedClient]") {
    if ( !isRevTrees() ) return;  //TEMP
    importJSONLines(sFixturesDir + "names_100.json");
    start();

    Encoder enc;
    enc.beginDict();
    enc["connected"] = "client";
    enc.endDict();
    auto docBody = enc.finish();

    Result<void> result;
    slice        curRevID = isRevTrees() ? "1-d00d" : "17@AliceAliceAliceAliceAA";
    slice        newRevID = isRevTrees() ? "2-2222" : "99@ZegpoldZegpoldZegpoldA";
    _client->putDoc(kC4DefaultCollectionSpec, "0000001", newRevID, curRevID, C4RevisionFlags{}, docBody,
                    this->expect(result));
    wait();

    REQUIRE(result.error() == C4Error{LiteCoreDomain, kC4ErrorConflict});
}

#pragma mark - OBSERVE:

#if 0  //TEMP

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "observeCollection", "[ConnectedClient]") {
    {
        // Start with a single doc that should not be sent to the observer
        TransactionHelper t(db);
        createFleeceRev(db, "doc1"_sl, "1-1111"_sl, R"({"name":"Puddin' Tane"})"_sl);
    }
    start();

    mutex                                m;
    condition_variable                   cond;
    vector<C4CollectionObserver::Change> allChanges;

    _client->observeCollection(nullslice,
                               [&](vector<C4CollectionObserver::Change> const& changes) {
                                   // Observer callback:
                                   unique_lock<mutex> lock(m);
                                   Log("+++ Observer got %zu changes!", changes.size());
                                   allChanges.insert(allChanges.end(), changes.begin(), changes.end());
                                   cond.notify_one();
                               })
            .then([&](C4Error error) {
                // Async callback when the observer has started:
                unique_lock<mutex> lock(m);
                REQUIRE(error == C4Error{});
                Log("+++ Importing docs...");
                importJSONLines(sFixturesDir + "names_100.json");
            });

    Log("+++ Waiting for 100 changes to arrive...");
    unique_lock<mutex> lock(m);
    cond.wait(lock, [&] { return allChanges.size() >= 100; });

    Log("+++ Checking the changes");
    REQUIRE(allChanges.size() == 100);
    C4SequenceNumber expectedSeq = 2;
    for ( auto& change : allChanges ) {
        CHECK(change.docID.size == 7);
        CHECK(change.flags == 0);
        CHECK(change.sequence == expectedSeq++);
    }
}

#    pragma mark - LEGACY ATTACHMENTS:

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getRev Blobs Legacy Mode", "[ConnectedClient][blob]") {
    static constexpr slice kJSON5WithAttachments =
            "{_attachments:{'blob_/attached/0':{content_type:'text/"
            "plain',digest:'sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=',length:27,revpos:1,stub:true},"
            "'blob_/attached/1':{content_type:'text/plain',digest:'sha1-rATs731fnP+PJv2Pm/"
            "WXWZsCw48=',length:10,revpos:1,stub:true},"
            "empty:{content_type:'text/plain',digest:'sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=',length:0,revpos:1,stub:true}},"
            "attached:[{'@type':'blob',content_type:'text/plain',digest:'sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=',length:27},"
            "{'@type':'blob',content_type:'text/plain',digest:'sha1-rATs731fnP+PJv2Pm/WXWZsCw48=',length:10}]}";
    static constexpr slice kJSON5WithoutAttachments =
            "{_attachments:{empty:{content_type:'text/plain',digest:'sha1-2jmj7l5rSw0yVb/vlWAYkK/"
            "YBwk=',length:0,revpos:1,stub:true}},"
            "attached:[{'@type':'blob',content_type:'text/plain',digest:'sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=',length:27},"
            "{'@type':'blob',content_type:'text/plain',digest:'sha1-rATs731fnP+PJv2Pm/WXWZsCw48=',length:10}]}";

    createFleeceRev(db, "att1"_sl, "1-1111"_sl, slice(json5(kJSON5WithAttachments)));

    // Ensure the 'server' (LiteCore replicator) will not strip the `_attachments` property:
    _serverOptions->setProperty("disable_blob_support"_sl, true);
    start();

    auto asyncResult = _client->getDoc("att1", nullslice, nullslice);
    auto rev         = waitForResponse(asyncResult);
    CHECK(rev.docID == "att1");
    Doc    doc(rev.body);
    Dict   props = doc.asDict();
    string json(props.toJSON5());
    replace(json, '"', '\'');
    CHECK(slice(json) == kJSON5WithoutAttachments);
}

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "putDoc Blobs Legacy Mode", "[ConnectedClient][blob]") {
    // Ensure the 'server' (LiteCore replicator) will not strip the `_attachments` property:
    _serverOptions->setProperty("disable_blob_support"_sl, true);
    start();

    // Register the blobs with the ConnectedClient delegate, by digest:
    _blobs["sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck="] = alloc_slice("Hey, this is an attachment!");
    _blobs["sha1-rATs731fnP+PJv2Pm/WXWZsCw48="] = alloc_slice("So is this");
    _blobs["sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk="] = alloc_slice("");

    // Construct the document body, and PUT it:
    string json =
            "{'attached':[{'@type':'blob','content_type':'text/"
            "plain','digest':'sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=','length':27},"
            "{'@type':'blob','content_type':'text/plain','digest':'sha1-rATs731fnP+PJv2Pm/WXWZsCw48=','length':10},"
            "{'@type':'blob','content_type':'text/plain','digest':'sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=','length':0}]}";
    replace(json, '\'', '"');
    auto rq = _client->putDoc("att1", nullslice, "1-1111", nullslice, C4RevisionFlags{}, Doc::fromJSON(json).data());
    rq.blockUntilReady();

    // All blobs should have been requested by the server and removed from the map:
    CHECK(_blobs.empty());

    // Now read the doc from the server's database:
    json = getDocJSON(db, "att1"_sl);
    replace(json, '"', '\'');
    CHECK(json
          == "{'_attachments':{'blob_/attached/0':{'content_type':'text/"
             "plain','digest':'sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=','length':27,'revpos':1,'stub':true},"
             "'blob_/attached/1':{'content_type':'text/plain','digest':'sha1-rATs731fnP+PJv2Pm/"
             "WXWZsCw48=','length':10,'revpos':1,'stub':true},"
             "'blob_/attached/2':{'content_type':'text/plain','digest':'sha1-2jmj7l5rSw0yVb/vlWAYkK/"
             "YBwk=','length':0,'revpos':1,'stub':true}},"
             "'attached':[{'@type':'blob','content_type':'text/"
             "plain','digest':'sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=','length':27},"
             "{'@type':'blob','content_type':'text/plain','digest':'sha1-rATs731fnP+PJv2Pm/WXWZsCw48=','length':10},"
             "{'@type':'blob','content_type':'text/plain','digest':'sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=','length':0}]}");
}

#    pragma mark - ALL-DOCS:

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "allDocs from connected client", "[ConnectedClient]") {
    importJSONLines(sFixturesDir + "names_100.json");
    start();

    mutex              mut;
    condition_variable cond;
    unique_lock<mutex> lock(mut);

    vector<string> results;

    _client->getAllDocIDs(nullslice, nullslice, [&](const vector<slice>& docIDs, const C4Error* error) {
        unique_lock<mutex> lock(mut);
        if ( !docIDs.empty() ) {
            Log("*** Got %zu docIDs", docIDs.size());
            CHECK(!error);
            for ( slice id : docIDs ) results.emplace_back(id);
        } else {
            Log("*** Got final row");
            if ( error ) results.push_back("Error: " + error->description());
            cond.notify_one();
        }
    });

    Log("Waiting for docIDs...");
    cond.wait(lock);
    Log("docIDs ready");
    CHECK(results.size() == 100);
}

#    pragma mark - ENCRYPTION:


C4UNUSED static constexpr slice
        kEncryptedDocJSON = R"({"encrypted$SSN":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"IzIzNC41Ni43ODk6Iw=="}})",
        kDecryptedDocJSON = R"({"SSN":{"@type":"encryptable","value":"123-45-6789"}})";

// Make sure there's no error if no decryption callback is given
TEST_CASE_METHOD(ConnectedClientLoopbackTest, "getRev encrypted no callback", "[ConnectedClient]") {
    createFleeceRev(db, "seekrit"_sl, "1-1111"_sl, kEncryptedDocJSON);
    start();

    Log("++++ Calling ConnectedClient::getDoc()...");
    auto asyncResult1 = _client->getDoc("seekrit", nullslice, nullslice);
    auto rev          = waitForResponse(asyncResult1);
    Doc  doc(rev.body);
    CHECK(doc.root().toJSONString() == string(kEncryptedDocJSON));
}

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "putDoc encrypted no callback", "[ConnectedClient]") {
    start();

    Doc doc = Doc::fromJSON(kDecryptedDocJSON);

    Log("++++ Calling ConnectedClient::putDoc()...");
    C4Error error = {};
    try {
        ExpectingExceptions x;
        auto rq1 = _client->putDoc("seekrit", nullslice, "1-1111", nullslice, C4RevisionFlags{}, doc.data());
    } catch ( const exception& x ) { error = C4Error::fromCurrentException(); }
    CHECK(error == C4Error{LiteCoreDomain, kC4ErrorCrypto});
}


#    ifdef COUCHBASE_ENTERPRISE

class ConnectedClientEncryptedLoopbackTest : public ConnectedClientLoopbackTest {
  public:
    ConnectedClientEncryptedLoopbackTest() {
        _params.propertyEncryptor = &encryptor;
        _params.propertyDecryptor = &decryptor;
        _params.callbackContext   = &_encryptorContext;
    }

    static alloc_slice unbreakableEncryption(slice cleartext, int8_t delta) {
        alloc_slice ciphertext(cleartext);
        for ( size_t i = 0; i < ciphertext.size; ++i )
            (uint8_t&)ciphertext[i] += delta;  // "I've got patent pending on that!" --Wallace
        return ciphertext;
    }

    struct TestEncryptorContext {
        slice docID;
        slice keyPath;
        bool  called = false;
    };

    TestEncryptorContext _encryptorContext;

    static C4SliceResult encryptor(void* rawCtx, C4CollectionSpec collection, C4String documentID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4StringResult* outAlgorithm,
                                   C4StringResult* outKeyID, C4Error* outError) {
        auto context    = (TestEncryptorContext*)rawCtx;
        context->called = true;
        CHECK(documentID == context->docID);
        CHECK(keyPath == context->keyPath);
        return C4SliceResult(unbreakableEncryption(input, 1));
    }

    static C4SliceResult decryptor(void* rawCtx, C4CollectionSpec collection, C4String documentID, FLDict properties,
                                   C4String keyPath, C4Slice input, C4String algorithm, C4String keyID,
                                   C4Error* outError) {
        auto context    = (TestEncryptorContext*)rawCtx;
        context->called = true;
        CHECK(documentID == context->docID);
        CHECK(keyPath == context->keyPath);
        return C4SliceResult(unbreakableEncryption(input, -1));
    }
};

TEST_CASE_METHOD(ConnectedClientEncryptedLoopbackTest, "getRev encrypted", "[ConnectedClient]") {
    createFleeceRev(db, "seekrit"_sl, "1-1111"_sl, kEncryptedDocJSON);
    start();

    _encryptorContext.docID   = "seekrit";
    _encryptorContext.keyPath = "SSN";

    Log("++++ Calling ConnectedClient::getDoc()...");
    auto asyncResult1 = _client->getDoc("seekrit", nullslice, nullslice);
    auto rev          = waitForResponse(asyncResult1);
    CHECK(_encryptorContext.called);
    Doc doc(rev.body);
    CHECK(doc.root().toJSON() == kDecryptedDocJSON);
}

TEST_CASE_METHOD(ConnectedClientEncryptedLoopbackTest, "putDoc encrypted", "[ConnectedClient]") {
    start();

    Doc doc = Doc::fromJSON(kDecryptedDocJSON);

    Log("++++ Calling ConnectedClient::getDoc()...");
    _encryptorContext.docID   = "seekrit";
    _encryptorContext.keyPath = "SSN";
    auto rq1 = _client->putDoc("seekrit", nullslice, "1-1111", nullslice, C4RevisionFlags{}, doc.data());
    rq1.blockUntilReady();
    CHECK(_encryptorContext.called);

    // Read the doc from the database to make sure it was encrypted.
    // Note that the replicator has no decryption callback so it will not decrypt the doc!
    c4::ref<C4Document> savedDoc = c4db_getDoc(db, "seekrit"_sl, true, kDocGetAll, ERROR_INFO());
    REQUIRE(savedDoc);
    alloc_slice json = c4doc_bodyAsJSON(savedDoc, true, ERROR_INFO());
    CHECK(json == kEncryptedDocJSON);
}

#    endif  // COUCHBASE_ENTERPRISE


#    pragma mark - QUERIES:


static constexpr slice kQueryStr =
        "SELECT name.first, name.last FROM _ WHERE gender='male' and contact.address.state=$STATE";

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "named query from connected client", "[ConnectedClient]") {
    importJSONLines(sFixturesDir + "names_100.json");

    MutableDict queries = fleece::MutableDict::newDict();
    queries["guysIn"]   = kQueryStr;
    _serverOptions->setProperty(kC4ReplicatorOptionNamedQueries, queries);

    start();

    mutex              mut;
    condition_variable cond;

    vector<string> jsonResults, fleeceResults;

    MutableDict params = fleece::MutableDict::newDict();
    params["STATE"]    = "CA";
    _client->query("guysIn", params, true, [&](slice json, fleece::Dict row, const C4Error* error) {
        if ( row ) {
            CHECK(!error);
            Log("*** Got query row: %s", row.toJSONString().c_str());
            jsonResults.push_back(string(json));
            fleeceResults.push_back(row.toJSONString());
        } else {
            Log("*** Got final row");
            if ( error ) fleeceResults.push_back("Error: " + error->description());
            unique_lock<mutex> lock(mut);
            cond.notify_one();
        }
    });

    Log("Waiting for query...");
    unique_lock<mutex> lock(mut);
    cond.wait(lock);
    Log("Query complete");
    vector<string> expectedResults{R"({"first":"Cleveland","last":"Bejcek"})",
                                   R"({"first":"Rico","last":"Hoopengardner"})"};
    CHECK(fleeceResults == expectedResults);
    CHECK(jsonResults == expectedResults);
}

TEST_CASE_METHOD(ConnectedClientLoopbackTest, "n1ql query from connected client", "[ConnectedClient]") {
    importJSONLines(sFixturesDir + "names_100.json");

    _serverOptions->setProperty(kC4ReplicatorOptionAllQueries, true);

    start();

    mutex              mut;
    condition_variable cond;

    vector<string> jsonResults, fleeceResults;

    MutableDict params = fleece::MutableDict::newDict();
    params["STATE"]    = "CA";
    _client->query(kQueryStr, params, true, [&](slice json, fleece::Dict row, const C4Error* error) {
        if ( row ) {
            CHECK(!error);
            Log("*** Got query row: %s", row.toJSONString().c_str());
            jsonResults.push_back(string(json));
            fleeceResults.push_back(row.toJSONString());
        } else {
            Log("*** Got final row");
            if ( error ) fleeceResults.push_back("Error: " + error->description());
            unique_lock<mutex> lock(mut);
            cond.notify_one();
        }
    });

    Log("Waiting for query...");
    unique_lock<mutex> lock(mut);
    cond.wait(lock);
    Log("Query complete");
    vector<string> expectedResults{R"({"first":"Cleveland","last":"Bejcek"})",
                                   R"({"first":"Rico","last":"Hoopengardner"})"};
    CHECK(fleeceResults == expectedResults);
    CHECK(jsonResults == expectedResults);
}

#endif  //TEMP
