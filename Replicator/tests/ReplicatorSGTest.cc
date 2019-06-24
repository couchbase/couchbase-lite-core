//
// ReplicatorSGTest.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "ReplicatorAPITest.hh"
#include "c4Document+Fleece.h"
#include "Stopwatch.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"
#include <unistd.h>

using namespace fleece;


/* REAL-REPLICATOR (SYNC GATEWAY) TESTS

 The tests below are tagged [.SyncServer] to keep them from running during normal testing.
 Instead, they have to be invoked manually via the Catch command-line option `[.SyncServer]`.
 This is because they require that an external replication server is running.

 The default URL the tests connect to is blip://localhost:4984/scratch/, but this can be
 overridden by setting environment vars REMOTE_HOST, REMOTE_PORT, REMOTE_DB.
 WARNING: The tests will erase this database (via the SG REST API.)

 Some tests connect to other databases by setting `_remoteDBName`. These have fixed contents.
 The directory Replicator/tests/data/ contains Sync Gateway config files and Walrus data files,
 so if you `cd` to that directory and enter `sync_gateway config.json` you should be good to go.
 (For more details, see the README.md file in that directory.)
 */


TEST_CASE_METHOD(ReplicatorAPITest, "API Auth Failure", "[.SyncServer]") {
    _remoteDBName = kProtectedDBName;
    replicate(kC4OneShot, kC4Disabled, false);
    CHECK(_callbackStatus.error.domain == WebSocketDomain);
    CHECK(_callbackStatus.error.code == 401);
    CHECK(_headers["Www-Authenticate"].asString() == "Basic realm=\"Couchbase Sync Gateway\""_sl);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API ExtraHeaders", "[.SyncServer]") {
    _remoteDBName = kProtectedDBName;

    // Use the extra-headers option to add HTTP Basic auth:
    Encoder enc;
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorOptionExtraHeaders));
    enc.beginDict();
    enc.writeKey("Authorization"_sl);
    enc.writeString("Basic cHVwc2hhdzpmcmFuaw=="_sl);  // that's user 'pupshaw', password 'frank'
    enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    replicate(kC4OneShot, kC4Disabled, true);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Empty DB", "[.SyncServer]") {
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Non-Empty DB", "[.SyncServer]") {
    importJSONLines(sFixturesDir + "names_100.json");
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Empty Doc", "[.SyncServer]") {
    Encoder enc;
    enc.beginDict();
    enc.endDict();
    alloc_slice body = enc.finish();
    createRev("doc"_sl, kRevID, body);

    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Big DB", "[.SyncServer]") {
    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json");
    replicate(kC4OneShot, kC4Disabled);
}


#if 0
TEST_CASE_METHOD(ReplicatorAPITest, "API Push Large-Docs DB", "[.SyncServer]") {
    importJSONLines(sFixturesDir + "en-wikipedia-articles-1000-1.json");
    replicate(kC4OneShot, kC4Disabled);
}
#endif


TEST_CASE_METHOD(ReplicatorAPITest, "API Push 5000 Changes", "[Push]") {
    string revID;
    {
        TransactionHelper t(db);
        revID = createNewRev(db, "Doc"_sl, nullslice, kFleeceBody);
    }
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Mutations --------");
    {
        TransactionHelper t(db);
        for (int i = 2; i <= 5000; ++i)
            revID = createNewRev(db, "Doc"_sl, slice(revID), kFleeceBody);
    }

    C4Log("-------- Second Replication --------");
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Pull", "[.SyncServer]") {
    _remoteDBName = kITunesDBName;
    replicate(kC4Disabled, kC4OneShot);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Pull With Indexes", "[.SyncServer]") {
    // Indexes slow down doc insertion, so they affect replicator performance.
    REQUIRE(c4db_createIndex(db, C4STR("Name"),   C4STR("[[\".Name\"]]"), kC4FullTextIndex, nullptr, nullptr));
    REQUIRE(c4db_createIndex(db, C4STR("Artist"), C4STR("[[\".Artist\"]]"), kC4ValueIndex, nullptr, nullptr));
    REQUIRE(c4db_createIndex(db, C4STR("Year"),   C4STR("[[\".Year\"]]"), kC4ValueIndex, nullptr, nullptr));

    _remoteDBName = kITunesDBName;
    replicate(kC4Disabled, kC4OneShot);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Continuous Push", "[.SyncServer]") {
    importJSONLines(sFixturesDir + "names_100.json");
    _stopWhenIdle = true;
    replicate(kC4Continuous, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Continuous Pull", "[.SyncServer]") {
    _remoteDBName = kITunesDBName;
    _stopWhenIdle = true;
    replicate(kC4Disabled, kC4Continuous);
}


TEST_CASE_METHOD(ReplicatorAPITest, "Push & Pull Deletion", "[.SyncServer]") {
    createRev("doc"_sl, kRevID, kFleeceBody);
    createRev("doc"_sl, kRev2ID, kEmptyFleeceBody, kRevDeleted);

    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Deleting and re-creating database --------");
    deleteAndRecreateDB();
    createRev("doc"_sl, kRevID, kFleeceBody);

    replicate(kC4Disabled, kC4OneShot);

    c4::ref<C4Document> doc = c4doc_get(db, "doc"_sl, true, nullptr);
    REQUIRE(doc);

    CHECK(doc->revID == kRev2ID);
    CHECK((doc->flags & kDocDeleted) != 0);
    CHECK((doc->selectedRev.flags & kRevDeleted) != 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == kRevID);
}


TEST_CASE_METHOD(ReplicatorAPITest, "Push & Pull Attachments", "[.SyncServer]") {
    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
    }

    C4Error error;
    c4::ref<C4Document> doc = c4doc_get(db, "att1"_sl, true, &error);
    REQUIRE(doc);
    alloc_slice before = c4doc_bodyAsJSON(doc, true, &error);
    doc = nullptr;
    C4Log("Original doc: %.*s", SPLAT(before));

    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Deleting and re-creating database --------");
    deleteAndRecreateDB();

    replicate(kC4Disabled, kC4OneShot);

    doc = c4doc_get(db, "att1"_sl, true, &error);
    REQUIRE(doc);
    alloc_slice after = c4doc_bodyAsJSON(doc, true, &error);
    C4Log("Pulled doc: %.*s", SPLAT(after));

    // Is the pulled identical to the original?
    CHECK(after == before);

    // Did we get all of its attachments?
    auto blobStore = c4db_getBlobStore(db, &error);
    for( auto key : blobKeys) {
        alloc_slice blob = c4blob_getContents(blobStore, key, &error);
        CHECK(blob);
    }
}


TEST_CASE_METHOD(ReplicatorAPITest, "Prove Attachments", "[.SyncServer]") {
    vector<string> attachments = {"Hey, this is an attachment!"};
    {
        TransactionHelper t(db);
        addDocWithAttachments("doc one"_sl, attachments, "text/plain");
    }
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Creating 2nd doc with same attachments --------");

    {
        TransactionHelper t(db);
        addDocWithAttachments("doc two"_sl, attachments, "text/plain");
    }
    // Pushing the second doc will cause Sync Gateway to ask for proof (send "proveAttachment")
    // instead of requesting the attachment itself, since it already has the attachment.
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Pull Big Attachments", "[.SyncServer]") {
    _remoteDBName = kImagesDBName;
    replicate(kC4Disabled, kC4OneShot);

    C4Error error;
    c4::ref<C4Document> doc = c4doc_get(db, "Abstract"_sl, true, &error);
    REQUIRE(doc);
    auto root = Value::fromData(doc->selectedRev.body).asDict();
    auto attach = root.get("_attachments"_sl).asDict().get("Abstract.jpg"_sl).asDict();
    REQUIRE(attach);
    CHECK(attach.get("content_type").asString() == "image/jpeg"_sl);
    slice digest = attach.get("digest").asString();
    CHECK(digest == "sha1-9g3HeOewh8//ctPcZkh03o+A+PQ="_sl);
    C4BlobKey blobKey;
    c4blob_keyFromString(digest, &blobKey);
    auto size = c4blob_getSize(c4db_getBlobStore(db, nullptr), blobKey);
    CHECK(size == 15198281);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Conflict", "[.SyncServer]") {
    const string originalRevID = "1-3cb9cfb09f3f0b5142e618553966ab73539b8888";
    importJSONLines(sFixturesDir + "names_100.json");
    replicate(kC4OneShot, kC4Disabled);

    sendRemoteRequest("PUT", "0000013", "{\"_rev\":\"" + originalRevID + "\","
                                          "\"serverSideUpdate\":true}");

    createRev("0000013"_sl, "2-f000"_sl, kFleeceBody);

    c4::ref<C4Document> doc = c4doc_get(db, C4STR("0000013"), true, nullptr);
    REQUIRE(doc);
	C4Slice revID = C4STR("2-f000");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectParentRevision(doc));
	revID = slice(originalRevID);
    CHECK(doc->selectedRev.revID == revID);
    CHECK(doc->selectedRev.body.size > 0);
    CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);

    C4Log("-------- Pushing Again (conflict) --------");
    _expectedDocPushErrors = {"0000013"};
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Pulling --------");
    _expectedDocPushErrors = { };
    _expectedDocPullErrors = {"0000013"};
    replicate(kC4Disabled, kC4OneShot);

    C4Log("-------- Checking Conflict --------");
    doc = c4doc_get(db, C4STR("0000013"), true, nullptr);
    REQUIRE(doc);
    CHECK((doc->flags & kDocConflicted) != 0);
	revID = C4STR("2-f000");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectParentRevision(doc));
	revID = slice(originalRevID);
    CHECK(doc->selectedRev.revID == revID);
#if 0 // FIX: These checks fail due to issue #402; re-enable when fixing that bug
    CHECK(doc->selectedRev.body.size > 0);
    CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);
#endif
    REQUIRE(c4doc_selectCurrentRevision(doc));
    REQUIRE(c4doc_selectNextRevision(doc));
	revID = C4STR("2-883a2dacc15171a466f76b9d2c39669b");
    CHECK(doc->selectedRev.revID == revID);
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectParentRevision(doc));
	revID = slice(originalRevID);
    CHECK(doc->selectedRev.revID == revID);
}


TEST_CASE_METHOD(ReplicatorAPITest, "Update Once-Conflicted Doc", "[.SyncServer]") {
    // For issue #448.
    // Create a conflicted doc on SG, and resolve the conflict:
    _remoteDBName = "scratch_allows_conflicts"_sl;
    flushScratchDatabase();
    sendRemoteRequest("PUT", "doc?new_edits=false", "{\"_rev\":\"1-aaaa\",\"foo\":1}"_sl);
    sendRemoteRequest("PUT", "doc?new_edits=false", "{\"_revisions\":{\"start\":2,\"ids\":[\"bbbb\",\"aaaa\"]},\"foo\":2.1}"_sl);
    sendRemoteRequest("PUT", "doc?new_edits=false", "{\"_revisions\":{\"start\":2,\"ids\":[\"cccc\",\"aaaa\"]},\"foo\":2.2}"_sl);
    sendRemoteRequest("PUT", "doc?new_edits=false", "{\"_revisions\":{\"start\":3,\"ids\":[\"dddd\",\"cccc\"]},\"_deleted\":true}"_sl);

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(kC4OneShot, kC4OneShot);

    // Verify doc:
    c4::ref<C4Document> doc = c4doc_get(db, "doc"_sl, true, nullptr);
    REQUIRE(doc);
	C4Slice revID = C4STR("2-bbbb");
    CHECK(doc->revID == revID);
    CHECK((doc->flags & kDocDeleted) == 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == "1-aaaa"_sl);

    // Update doc:
    createRev("doc"_sl, "3-ffff"_sl, kFleeceBody);

    // Push change back to SG:
    C4Log("-------- Pushing");
    replicate(kC4OneShot, kC4OneShot);

    // Verify doc is updated on SG:
    auto body = sendRemoteRequest("GET", "doc");
	C4Slice bodySlice = C4STR("{\"_id\":\"doc\",\"_rev\":\"3-ffff\",\"ans*wer\":42}");
    CHECK(C4Slice(body) == bodySlice);
}


TEST_CASE_METHOD(ReplicatorAPITest, "Pull multiply-updated", "[.SyncServer]") {
    // From <https://github.com/couchbase/couchbase-lite-core/issues/652>:
    // 1. Setup CB cluster & Configure SG
    // 2. Create a document using POST API via SG
    // 3. Create a cblite db on local server using cblite serve
    //      ./cblite/build/cblite serve  --create db.cblite2
    // 4. Replicate between SG -> db.cblite2
    //      ./cblite/build/cblite pull  ws://172.23.100.204:4985/db db.cblite2
    // 5. Validate number of records on db.cblite2 ->Should be  equal to number of documents created in Step2
    // 6. Update existing document using update API via SG (more than twice)
    //      PUT sghost:4985/bd/doc_id?=rev_id
    // 7. run replication between SG -> db.cblite2 again

    flushScratchDatabase();
    sendRemoteRequest("PUT", "doc?new_edits=false", "{\"count\":1, \"_rev\":\"1-1111\"}"_sl);

    replicate(kC4Disabled, kC4OneShot);
    c4::ref<C4Document> doc = c4doc_get(db, "doc"_sl, true, nullptr);
    REQUIRE(doc);
    CHECK(doc->revID == "1-1111"_sl);

    sendRemoteRequest("PUT", "doc", "{\"count\":2, \"_rev\":\"1-1111\"}"_sl);
    sendRemoteRequest("PUT", "doc", "{\"count\":3, \"_rev\":\"2-c5557c751fcbfe4cd1f7221085d9ff70\"}"_sl);
    sendRemoteRequest("PUT", "doc", "{\"count\":4, \"_rev\":\"3-2284e35327a3628df1ca8161edc78999\"}"_sl);

    replicate(kC4Disabled, kC4OneShot);
    doc = c4doc_get(db, "doc"_sl, true, nullptr);
    REQUIRE(doc);
    CHECK(doc->revID == "4-ffa3011c5ade4ec3a3ec5fe2296605ce"_sl);
}


TEST_CASE_METHOD(ReplicatorAPITest, "Pull deltas from SG", "[.SyncServer][Delta]") {
    static constexpr int kNumDocs = 1000, kNumProps = 1000;
    flushScratchDatabase();
    _logRemoteRequests = false;

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        TransactionHelper t(db);
        srandom(123456); // start random() sequence at a known place
        for (int docNo = 0; docNo < kNumDocs; ++docNo) {
            char docID[20];
            sprintf(docID, "doc-%03d", docNo);
            Encoder enc(c4db_createFleeceEncoder(db));
            enc.beginDict();
            for (int p = 0; p < kNumProps; ++p) {
                enc.writeKey(format("field%03d", p));
                enc.writeInt(random());
            }
            enc.endDict();
            alloc_slice body = enc.finish();
            string revID = createNewRev(db, slice(docID), body);
        }
    };
    populateDB();

    C4Log("-------- Pushing to SG --------");
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Updating docs on SG --------");
    // Now update the docs on SG:
    {
        JSONEncoder enc;
        enc.beginDict();
        enc.writeKey("docs"_sl);
        enc.beginArray();
        for (int docNo = 0; docNo < kNumDocs; ++docNo) {
            char docID[20];
            sprintf(docID, "doc-%03d", docNo);
            C4Error error;
            c4::ref<C4Document> doc = c4doc_get(db, slice(docID), false, &error);
            REQUIRE(doc);
            Dict props = Value::fromData(doc->selectedRev.body).asDict();

            enc.beginDict();
            enc.writeKey("_id"_sl);
            enc.writeString(docID);
            enc.writeKey("_rev"_sl);
            enc.writeString(doc->revID);
            for (Dict::iterator i(props); i; ++i) {
                enc.writeKey(i.keyString());
                auto value = i.value().asInt();
                if (random() % 8 == 0)
                    value = random();
                enc.writeInt(value);
            }
            enc.endDict();
        }
        enc.endArray();
        enc.endDict();
        sendRemoteRequest("POST", "_bulk_docs", enc.finish());
    }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for (int pass = 1; pass <= 3; ++pass) {
        if (pass == 3) {
            C4Log("-------- DISABLING DELTA SYNC --------");
            Encoder enc;
            enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorOptionDisableDeltas));
            enc.writeBool(true);
            enc.endDict();
            _options = AllocedDict(enc.finish());
        }

        C4Log("-------- PASS #%d: Repopulating local db --------", pass);
        deleteAndRecreateDB();
        populateDB();
        C4Log("-------- PASS #%d: Pulling changes from SG --------", pass);
        Stopwatch st;
        replicate(kC4Disabled, kC4OneShot);
        double time = st.elapsed();
        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, kNumDocs/time);
        if (pass == 2)
            timeWithDelta = time;
        else if (pass == 3)
            timeWithoutDelta = time;

        int n = 0;
        C4Error error;
        c4::ref<C4DocEnumerator> e = c4db_enumerateAllDocs(db, nullptr, &error);
        while (c4enum_next(e, &error)) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            CHECK(slice(info.docID).hasPrefix("doc-"_sl));
            CHECK(slice(info.revID).hasPrefix("2-"_sl));
            ++n;
        }
        CHECK(error.code == 0);
        CHECK(n == kNumDocs);
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed",
          timeWithDelta, timeWithoutDelta, timeWithoutDelta/timeWithDelta);
}


TEST_CASE_METHOD(ReplicatorAPITest, "Pull iTunes deltas from SG", "[.SyncServer][Delta]") {
    flushScratchDatabase();
    _logRemoteRequests = false;

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        TransactionHelper t(db);
        importJSONLines(sFixturesDir + "iTunesMusicLibrary.json");
    };
    populateDB();
    auto numDocs = c4db_getDocumentCount(db);

    C4Log("-------- Pushing to SG --------");
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Updating docs on SG --------");
    // Now update the docs on SG:
    {
        JSONEncoder enc;
        enc.beginDict();
        enc.writeKey("docs"_sl);
        enc.beginArray();
        for (int docNo = 0; docNo < numDocs; ++docNo) {
            char docID[20];
            sprintf(docID, "%07u", docNo + 1);
            C4Error error;
            c4::ref<C4Document> doc = c4doc_get(db, slice(docID), false, &error);
            REQUIRE(doc);
            Dict props = Value::fromData(doc->selectedRev.body).asDict();

            enc.beginDict();
            enc.writeKey("_id"_sl);
            enc.writeString(docID);
            enc.writeKey("_rev"_sl);
            enc.writeString(doc->revID);
            for (Dict::iterator i(props); i; ++i) {
                enc.writeKey(i.keyString());
                auto value = i.value();
                if (i.keyString() == "Play Count"_sl)
                    enc.writeInt(value.asInt() + 1);
                else
                    enc.writeValue(value);
            }
            enc.endDict();
        }
        enc.endArray();
        enc.endDict();
        sendRemoteRequest("POST", "_bulk_docs", enc.finish());
    }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for (int pass = 1; pass <= 3; ++pass) {
        if (pass == 3) {
            C4Log("-------- DISABLING DELTA SYNC --------");
            Encoder enc;
            enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorOptionDisableDeltas));
            enc.writeBool(true);
            enc.endDict();
            _options = AllocedDict(enc.finish());
        }

        C4Log("-------- PASS #%d: Repopulating local db --------", pass);
        deleteAndRecreateDB();
        populateDB();
        C4Log("-------- PASS #%d: Pulling changes from SG --------", pass);
        Stopwatch st;
        replicate(kC4Disabled, kC4OneShot);
        double time = st.elapsed();
        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, numDocs/time);
        if (pass == 2)
            timeWithDelta = time;
        else if (pass == 3)
            timeWithoutDelta = time;

        int n = 0;
        C4Error error;
        c4::ref<C4DocEnumerator> e = c4db_enumerateAllDocs(db, nullptr, &error);
        while (c4enum_next(e, &error)) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            //CHECK(slice(info.docID).hasPrefix("doc-"_sl));
            CHECK(slice(info.revID).hasPrefix("2-"_sl));
            ++n;
        }
        CHECK(error.code == 0);
        CHECK(n == numDocs);
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed",
          timeWithDelta, timeWithoutDelta, timeWithoutDelta/timeWithDelta);
}
