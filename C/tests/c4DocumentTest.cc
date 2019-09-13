//
// c4DocumentTest.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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
#include "c4Document+Fleece.h"
#include "c4Private.h"
#include "Benchmark.hh"
#include "fleece/Fleece.hh"

using namespace fleece;


TEST_CASE("Generate docID", "[Database][C]") {
    char buf[kC4GeneratedIDLength + 1];
    CHECK(c4doc_generateID(buf, 0) == nullptr);
    CHECK(c4doc_generateID(buf, kC4GeneratedIDLength) == nullptr);
    for (int pass = 0; pass < 10; ++pass) {
        REQUIRE(c4doc_generateID(buf, sizeof(buf)) == buf);
        C4Log("docID = '%s'", buf);
        REQUIRE(strlen(buf) == kC4GeneratedIDLength);
        CHECK(buf[0] == '~');
        for (int i = 1; i < strlen(buf); ++i) {
            bool validChar = isalpha(buf[i]) || isdigit(buf[i]) || buf[i] == '_' || buf[i] == '-';
            CHECK(validChar);
        }
    }
}


N_WAY_TEST_CASE_METHOD(C4Test, "Invalid docID", "[Database][C]") {
    c4log_warnOnErrors(false);
    TransactionHelper t(db);

    auto checkPutBadDocID = [this](C4Slice docID) {
        C4Error error;
        C4DocPutRequest rq = {};
        rq.body = C4Test::kFleeceBody;
        rq.save = true;
        rq.docID = docID;
        CHECK(c4doc_put(db, &rq, nullptr, &error) == nullptr);
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorBadDocID);
    };

    SECTION("empty") {
        checkPutBadDocID(C4STR(""));
    }
    SECTION("too long") {
        char buf[241];
        memset(buf, 'x', sizeof(buf));
        checkPutBadDocID({buf, sizeof(buf)});
    }
    SECTION("bad UTF-8") {
        checkPutBadDocID(C4STR("oops\x00oops"));
    }
    SECTION("control character") {
        checkPutBadDocID(C4STR("oops\noops"));
    }
    c4log_warnOnErrors(true);
}


N_WAY_TEST_CASE_METHOD(C4Test, "FleeceDocs", "[Document][Fleece][C]") {
    importJSONLines(sFixturesDir + "names_100.json");
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document PossibleAncestors", "[Document][C]") {
    if (!isRevTrees()) return;

    createRev(kDocID, kRevID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody);
    createRev(kDocID, kRev3ID, kFleeceBody);

    C4Document *doc = c4doc_get(db, kDocID, true, NULL);
    REQUIRE(doc);

    C4Slice newRevID = C4STR("3-f00f00");
    REQUIRE(c4doc_selectFirstPossibleAncestorOf(doc, newRevID));
    REQUIRE(doc->selectedRev.revID == kRev2ID);
    REQUIRE(c4doc_selectNextPossibleAncestorOf(doc, newRevID));
    REQUIRE(doc->selectedRev.revID == kRevID);
    REQUIRE(!c4doc_selectNextPossibleAncestorOf(doc, newRevID));

    newRevID = C4STR("2-f00f00");
    REQUIRE(c4doc_selectFirstPossibleAncestorOf(doc, newRevID));
    REQUIRE(doc->selectedRev.revID == kRevID);
    REQUIRE(!c4doc_selectNextPossibleAncestorOf(doc, newRevID));

    newRevID = C4STR("1-f00f00");
    REQUIRE(!c4doc_selectFirstPossibleAncestorOf(doc, newRevID));
    c4doc_release(doc);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document FindDocAncestors", "[Document][C]") {
    if (!isRevTrees()) return;

    C4String doc1 = C4STR("doc1"), doc2 = C4STR("doc2"), doc3 = C4STR("doc3");
    createRev(doc1, kRevID, kFleeceBody);
    createRev(doc1, kRev2ID, kFleeceBody);
    createRev(doc1, kRev3ID, kFleeceBody);

    createRev(doc2, kRevID, kFleeceBody);
    createRev(doc2, kRev2ID, kFleeceBody);
    createRev(doc2, kRev3ID, kFleeceBody);

    createRev(doc3, kRevID, kFleeceBody);
    createRev(doc3, kRev2ID, kFleeceBody);
    createRev(doc3, kRev3ID, kFleeceBody);

    C4SliceResult ancestors[4] = {};
    C4Error error;

    unsigned maxResults = 10;
    bool bodies = false;
    C4RemoteID remote = 1;

    auto toString = [](C4SliceResult sr) {return std::string(alloc_slice(sr));};

    // Doc I don't have yet:
    C4String newDocID = "new"_sl;
    REQUIRE(c4db_findDocAncestors(db, 1, maxResults, bodies, remote, &newDocID, &kRev3ID, ancestors, &error));
    CHECK(ancestors[0].size == 0);
    CHECK(ancestors[0].buf == 0);       // empty slice

    // Revision I already have:
    REQUIRE(c4db_findDocAncestors(db, 1, maxResults, bodies, remote, &doc1, &kRev3ID, ancestors, &error));
    CHECK(alloc_slice(ancestors[0]) == kC4AncestorExists);       // null slice

    // Newer revision:
    C4String newRevID = "4-deadbeef"_sl;
    REQUIRE(c4db_findDocAncestors(db, 1, maxResults, bodies, remote, &doc1, &newRevID, ancestors, &error));
    CHECK(toString(ancestors[0]) == R"(["3-deadbeef","2-c001d00d","1-abcd"])");

    // Conflict:
    newRevID = "3-00000000"_sl;
    REQUIRE(c4db_findDocAncestors(db, 1, maxResults, bodies, remote, &doc1, &newRevID, ancestors, &error));
    CHECK(toString(ancestors[0]) == R"(["2-c001d00d","1-abcd"])");

    // Require bodies:
    newRevID = "4-deadbeef"_sl;
    REQUIRE(c4db_findDocAncestors(db, 1, maxResults, true, remote, &doc1, &newRevID, ancestors, &error));
    CHECK(toString(ancestors[0]) == R"(["3-deadbeef"])");

    // Limit number of results:
    newRevID = "4-deadbeef"_sl;
    REQUIRE(c4db_findDocAncestors(db, 1, 1, bodies, remote, &doc1, &newRevID, ancestors, &error));
    CHECK(toString(ancestors[0]) == R"(["3-deadbeef"])");

    // Multiple docs:
    C4String docIDs[4] = {doc2,            doc1,    C4STR("doc4"),    doc3};
    C4String revIDs[4] = {"4-deadbeef"_sl, kRev3ID, C4STR("17-eeee"), "2-f000"_sl};
    REQUIRE(c4db_findDocAncestors(db, 4, maxResults, bodies, remote, docIDs, revIDs, ancestors, &error));
    CHECK(toString(ancestors[0]) == R"(["3-deadbeef","2-c001d00d","1-abcd"])");
    CHECK(alloc_slice(ancestors[1]) == kC4AncestorExists);
    CHECK(!slice(ancestors[2]));
    CHECK(toString(ancestors[3]) == R"(["1-abcd"])");
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document CreateVersionedDoc", "[Database][C]") {
    // Try reading doc with mustExist=true, which should fail:
    C4Error error;
    C4Document* doc;
    doc = c4doc_get(db, kDocID, true, &error);
    REQUIRE(!doc);
    REQUIRE((uint32_t)error.domain == (uint32_t)LiteCoreDomain);
    REQUIRE(error.code == (int)kC4ErrorNotFound);
    c4doc_release(doc);

    // Test c4doc_getSingleRevision, which also fails:
    doc = c4doc_getSingleRevision(db, kDocID, kC4SliceNull, false, &error);
    REQUIRE(!doc);
    REQUIRE((uint32_t)error.domain == (uint32_t)LiteCoreDomain);
    REQUIRE(error.code == (int)kC4ErrorNotFound);
    doc = c4doc_getSingleRevision(db, kDocID, kC4SliceNull, true, &error);
    REQUIRE(!doc);
    REQUIRE((uint32_t)error.domain == (uint32_t)LiteCoreDomain);
    REQUIRE(error.code == (int)kC4ErrorNotFound);
    doc = c4doc_getSingleRevision(db, kDocID, kRevID, true, &error);
    REQUIRE(!doc);
    REQUIRE((uint32_t)error.domain == (uint32_t)LiteCoreDomain);
    REQUIRE(error.code == (int)kC4ErrorNotFound);

    // Now get the doc with mustExist=false, which returns an empty doc:
    doc = c4doc_get(db, kDocID, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->flags == 0);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID.buf == 0);
    REQUIRE(doc->selectedRev.revID.buf == 0);
    c4doc_release(doc);

    {
        TransactionHelper t(db);
        C4DocPutRequest rq = {};
        rq.revFlags = kRevKeepBody;
        rq.existingRevision = true;
        rq.docID = kDocID;
        rq.history = &kRevID;
        rq.historyCount = 1;
        rq.body = kFleeceBody;
        rq.save = true;
        doc = c4doc_put(db, &rq, nullptr, &error);
        REQUIRE(doc != nullptr);
        REQUIRE(doc->revID == kRevID);
        REQUIRE(doc->selectedRev.revID == kRevID);
        REQUIRE(doc->selectedRev.flags == (kRevKeepBody | kRevLeaf));
        REQUIRE(doc->selectedRev.body == kFleeceBody);
        c4doc_release(doc);
    }

    // Reload the doc:
    doc = c4doc_get(db, kDocID, true, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->sequence == 1);
    REQUIRE(doc->flags == kDocExists);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRevID);
    REQUIRE(doc->selectedRev.revID == kRevID);
    REQUIRE(doc->selectedRev.sequence == 1);
    REQUIRE(doc->selectedRev.body == kFleeceBody);
    c4doc_release(doc);

    // Get the doc by its sequence:
    doc = c4doc_getBySequence(db, 1, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->sequence == 1);
    REQUIRE(doc->flags == kDocExists);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRevID);
    REQUIRE(doc->selectedRev.revID == kRevID);
    REQUIRE(doc->selectedRev.sequence == 1);
    REQUIRE(doc->selectedRev.body == kFleeceBody);
    {
        TransactionHelper t(db);
        REQUIRE(c4doc_removeRevisionBody(doc));
        REQUIRE(c4doc_selectCurrentRevision(doc));
    }
    
    REQUIRE(doc->selectedRev.body.buf == nullptr);
    REQUIRE(doc->selectedRev.body.size == 0);
    c4doc_release(doc);

    // Get a bogus sequence
    doc = c4doc_getBySequence(db, 2, &error);
    CHECK(doc == nullptr);
    CHECK(error.domain == LiteCoreDomain);
    CHECK(error.code == kC4ErrorNotFound);

    // Test c4doc_getSingleRevision (without body):
    doc = c4doc_getSingleRevision(db, kDocID, kC4SliceNull, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->sequence == 1);
    REQUIRE(doc->flags == kDocExists);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRevID);
    REQUIRE(doc->selectedRev.revID == kRevID);
    REQUIRE(doc->selectedRev.sequence == 1);
    REQUIRE(doc->selectedRev.body == kC4SliceNull);
    c4doc_release(doc);

    // Test c4doc_getSingleRevision (with body):
    doc = c4doc_getSingleRevision(db, kDocID, kC4SliceNull, true, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->sequence == 1);
    REQUIRE(doc->flags == kDocExists);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRevID);
    REQUIRE(doc->selectedRev.revID == kRevID);
    REQUIRE(doc->selectedRev.sequence == 1);
    REQUIRE(doc->selectedRev.body == kFleeceBody);
    c4doc_release(doc);

    // Test c4doc_getSingleRevision (with specific rev):
    doc = c4doc_getSingleRevision(db, kDocID, kRevID, true, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->sequence == 1);
    REQUIRE(doc->flags == kDocExists);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRevID);
    REQUIRE(doc->selectedRev.revID == kRevID);
    REQUIRE(doc->selectedRev.sequence == 1);
    REQUIRE(doc->selectedRev.body == kFleeceBody);
    c4doc_release(doc);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document CreateMultipleRevisions", "[Database][C]") {
    const auto kFleeceBody2 = json2fleece("{'ok':'go'}");
    const auto kFleeceBody3 = json2fleece("{'ubu':'roi'}");
    createRev(kDocID, kRevID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody2, kRevKeepBody);
    createRev(kDocID, kRev2ID, kFleeceBody2); // test redundant insert

    // Reload the doc:
    C4Error error;
    C4Document *doc = c4doc_get(db, kDocID, true, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->flags == kDocExists);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRev2ID);
    REQUIRE(doc->selectedRev.revID == kRev2ID);
    REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)2);
    REQUIRE(doc->selectedRev.body == kFleeceBody2);

    if (versioning() == kC4RevisionTrees) {
        // Select 1st revision:
        REQUIRE(c4doc_selectParentRevision(doc));
        REQUIRE(doc->selectedRev.revID == kRevID);
        REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)1);
        REQUIRE(doc->selectedRev.body == kC4SliceNull);
        REQUIRE(!c4doc_hasRevisionBody(doc));
        REQUIRE(!c4doc_selectParentRevision(doc));
        c4doc_release(doc);

        // Add a 3rd revision:
        createRev(kDocID, kRev3ID, kFleeceBody3);
        // Revision 2 should keep its body due to the kRevKeepBody flag:
        doc = c4doc_get(db, kDocID, true, &error);
        REQUIRE(doc != nullptr);
        REQUIRE(c4doc_selectParentRevision(doc));
        REQUIRE(doc->selectedRev.revID == kRev2ID);
        REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)2);
        REQUIRE(doc->selectedRev.flags == kRevKeepBody);
        REQUIRE(doc->selectedRev.body == kFleeceBody2);
        c4doc_release(doc);

        // Test c4doc_getSingleRevision (with body):
        doc = c4doc_getSingleRevision(db, kDocID, kC4SliceNull, true, &error);
        REQUIRE(doc != nullptr);
        REQUIRE(doc->sequence == 3);
        REQUIRE(doc->flags == kDocExists);
        REQUIRE(doc->docID == kDocID);
        REQUIRE(doc->revID == kRev3ID);
        REQUIRE(doc->selectedRev.revID == kRev3ID);
        REQUIRE(doc->selectedRev.sequence == 3);
        REQUIRE(doc->selectedRev.body == kFleeceBody3);
        c4doc_release(doc);

        // Test c4doc_getSingleRevision (with specific revision):
        doc = c4doc_getSingleRevision(db, kDocID, kRev2ID, true, &error);
        REQUIRE(doc != nullptr);
        REQUIRE(doc->sequence == 3);
        REQUIRE(doc->flags == kDocExists);
        REQUIRE(doc->docID == kDocID);
        REQUIRE(doc->revID == kRev3ID);
        REQUIRE(doc->selectedRev.revID == kRev2ID);
        REQUIRE(doc->selectedRev.sequence == 2);
        REQUIRE(doc->selectedRev.body == kFleeceBody2);
        c4doc_release(doc);

        // Purge doc
        {
            TransactionHelper t(db);
            doc = c4doc_get(db, kDocID, true, &error);
            int nPurged = c4doc_purgeRevision(doc, {}, &error);
            REQUIRE(nPurged == 3);
            REQUIRE(c4doc_save(doc, 20, &error));
            c4doc_release(doc);
        }

        // Make sure it's gone:
        doc = c4doc_get(db, kDocID, true, &error);
        CHECK(!doc);
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorNotFound);
    }
    c4doc_release(doc);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Get Single Revision", "[Document][C]") {
    if (!isRevTrees()) return;

    createRev(kDocID, kRevID, kEmptyFleeceBody);
    createRev(kDocID, kRev2ID, kEmptyFleeceBody);
    createRev(kDocID, kRev3ID, kFleeceBody);

    C4Error error;
    for (int withBody = false; withBody <= true; ++withBody) {
        C4Document *doc = c4doc_getSingleRevision(db, kDocID, nullslice, withBody, &error);
        REQUIRE(doc);
        CHECK(doc->sequence == 3);
        CHECK(doc->flags == kDocExists);
        CHECK(doc->docID == kDocID);
        CHECK(doc->revID == kRev3ID);
        CHECK(doc->selectedRev.revID == kRev3ID);
        CHECK(doc->selectedRev.sequence == 3);
        if (withBody)
            CHECK(doc->selectedRev.body == kFleeceBody);
        else
            CHECK(doc->selectedRev.body == nullslice);
        c4doc_release(doc);
    }

    C4Document *doc = c4doc_getSingleRevision(db, kDocID, "99-ffff"_sl, true, &error);
    CHECK(!doc);
    CHECK(error.domain == LiteCoreDomain);
    CHECK(error.code == kC4ErrorNotFound);

    doc = c4doc_getSingleRevision(db, "missing"_sl, nullslice, true, &error);
    CHECK(!doc);
    CHECK(error.domain == LiteCoreDomain);
    CHECK(error.code == kC4ErrorNotFound);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Purge", "[Database][C]") {
    const auto kFleeceBody2 = json2fleece("{'ok':'go'}");
    const auto kFleeceBody3 = json2fleece("{'ubu':'roi'}");
    createRev(kDocID, kRevID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody2);
    createRev(kDocID, kRev3ID, kFleeceBody3);
    
    C4Slice history[3] = {C4STR("3-ababab"), kRev2ID};
    C4DocPutRequest rq = {};
    rq.existingRevision = true;
    rq.docID = kDocID;
    rq.history = history;
    rq.historyCount = 2;
    rq.allowConflict = true;
    rq.body = kFleeceBody3;
    rq.save = true;
    C4Error err;
    REQUIRE(c4db_beginTransaction(db, &err));
    auto doc = c4doc_put(db, &rq, nullptr, &err);
    REQUIRE(doc);
    c4doc_release(doc);
    REQUIRE(c4db_endTransaction(db, true, &err));
    
    REQUIRE(c4db_beginTransaction(db, &err));
    REQUIRE(c4db_purgeDoc(db, kDocID, &err));
    REQUIRE(c4db_endTransaction(db, true, &err));
    
    REQUIRE(c4db_getDocumentCount(db) == 0);
    
    createRev(kDocID, kRevID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody2);
    createRev(kDocID, kRev3ID, kFleeceBody3);
    REQUIRE(c4db_beginTransaction(db, &err));
    doc = c4doc_put(db, &rq, nullptr, &err);
    REQUIRE(doc);
    REQUIRE(c4db_endTransaction(db, true, &err));
    
    REQUIRE(c4db_beginTransaction(db, &err));
    CHECK(c4doc_purgeRevision(doc, kRev2ID, &err) == 0);
    REQUIRE(c4doc_purgeRevision(doc, kC4SliceNull, &err) == 4);
    REQUIRE(c4doc_save(doc, 20, &err));
    c4doc_release(doc);
    REQUIRE(c4db_endTransaction(db, true, &err));
    REQUIRE(c4db_getDocumentCount(db) == 0);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document maxRevTreeDepth", "[Database][C]") {
    if (isRevTrees()) {
        CHECK(c4db_getMaxRevTreeDepth(db) == 20);
        c4db_setMaxRevTreeDepth(db, 30);
        CHECK(c4db_getMaxRevTreeDepth(db) == 30);
        reopenDB();
        CHECK(c4db_getMaxRevTreeDepth(db) == 30);
    }

    for (int setRemoteOrigin = 0; setRemoteOrigin <= 1; ++setRemoteOrigin) {
        C4Log("-------- setRemoteOrigin = %d", setRemoteOrigin);
        static const unsigned kNumRevs = 10000;
        fleece::Stopwatch st;
        C4Error error;
        C4String docID;
    	if(setRemoteOrigin) {
    		docID = C4STR("doc_noRemote");
		} else {
			docID = C4STR("doc_withRemote");
		}

        auto doc = c4doc_get(db, docID, false, &error);
        {
            TransactionHelper t(db);
            REQUIRE(doc != nullptr);
            for (unsigned i = 0; i < kNumRevs; i++) {
                C4DocPutRequest rq = {};
                rq.docID = doc->docID;
                rq.history = &doc->revID;
                rq.historyCount = 1;
                rq.body = kFleeceBody;
                if (setRemoteOrigin && i == 0) {
                    // Pretend the 1st revision has a remote origin (see issue #376)
                    rq.remoteDBID = 1;
                    rq.existingRevision = true;
                    rq.history = &kRevID;
                }
                rq.save = true;
                auto savedDoc = c4doc_put(db, &rq, nullptr, &error);
                REQUIRE(savedDoc != nullptr);
                c4doc_release(doc);
                doc = savedDoc;
            }
        }
        C4Log("Created %u revisions in %.3f sec", kNumRevs, st.elapsed());

        // Check rev tree depth:
        unsigned nRevs = 0;
        c4doc_selectCurrentRevision(doc);
        do {
            if (isRevTrees()) {
                unsigned expectedGen = kNumRevs - nRevs;
                if (setRemoteOrigin && nRevs == 30)
                    expectedGen = 1; // the remote-origin rev is pinned
                CHECK(c4rev_getGeneration(doc->selectedRev.revID) == expectedGen);
            }
            ++nRevs;
        } while (c4doc_selectParentRevision(doc));
        C4Log("Document rev tree depth is %u", nRevs);
        if (isRevTrees())
            REQUIRE(nRevs == (setRemoteOrigin ? 31 : 30));

        c4doc_release(doc);
    }
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document GetForPut", "[Database][C]") {
    C4Error error;
    TransactionHelper t(db);

    // Creating doc given ID:
    auto doc = c4doc_getForPut(db, kDocID, kC4SliceNull, false, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kC4SliceNull);
    REQUIRE(doc->flags == 0);
    REQUIRE(doc->selectedRev.revID == kC4SliceNull);
    c4doc_release(doc);

    // Creating doc, no ID:
    doc = c4doc_getForPut(db, kC4SliceNull, kC4SliceNull, false, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID.size >= 20);  // Verify it got a random doc ID
    REQUIRE(doc->revID == kC4SliceNull);
    REQUIRE(doc->flags == 0);
    REQUIRE(doc->selectedRev.revID == kC4SliceNull);
    c4doc_release(doc);

    // Delete with no revID given
    doc = c4doc_getForPut(db, kDocID, kC4SliceNull, true/*deleting*/, false, &error);
    REQUIRE(doc == nullptr);
    REQUIRE(error.code == kC4ErrorNotFound);

    // Adding new rev of nonexistent doc:
    doc = c4doc_getForPut(db, kDocID, kRevID, false, false, &error);
    REQUIRE(doc == nullptr);
    REQUIRE(error.code == kC4ErrorNotFound);

    // Adding new rev of existing doc:
    createRev(kDocID, kRevID, kFleeceBody);
    doc = c4doc_getForPut(db, kDocID, kRevID, false, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRevID);
    REQUIRE(doc->flags == kDocExists);
    REQUIRE(doc->selectedRev.revID == kRevID);
    c4doc_release(doc);

    // Adding new rev, with nonexistent parent:
    doc = c4doc_getForPut(db, kDocID, kRev2ID, false, false, &error);
    REQUIRE(doc == nullptr);
    REQUIRE(error.code == kC4ErrorConflict);

    // Conflict -- try & fail to update non-current rev:
    const auto kFleeceBody2 = json2fleece("{'ok':'go'}");
    createRev(kDocID, kRev2ID, kFleeceBody2);
    doc = c4doc_getForPut(db, kDocID, kRevID, false, false, &error);
    REQUIRE(doc == nullptr);
    REQUIRE(error.code == kC4ErrorConflict);

    if (isRevTrees()) {
        // Conflict -- force an update of non-current rev:
        doc = c4doc_getForPut(db, kDocID, kRevID, false, true/*allowConflicts*/, &error);
        REQUIRE(doc != nullptr);
        REQUIRE(doc->docID == kDocID);
        REQUIRE(doc->selectedRev.revID == kRevID);
        c4doc_release(doc);
    }

    // Deleting the doc:
    doc = c4doc_getForPut(db, kDocID, kRev2ID, true/*deleted*/, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->selectedRev.revID == kRev2ID);
    c4doc_release(doc);
    
    // Actually delete it:
    createRev(kDocID, kRev3ID, kC4SliceNull, kRevDeleted);

    // Re-creating the doc (no revID given):
    doc = c4doc_getForPut(db, kDocID, kC4SliceNull, false, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRev3ID);
    REQUIRE(doc->flags == (kDocExists | kDocDeleted));
    REQUIRE(doc->selectedRev.revID == kRev3ID);
    c4doc_release(doc);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Put", "[Database][C]") {
    C4Error error;
    TransactionHelper t(db);

    // Creating doc given ID:
    C4DocPutRequest rq = {};
    rq.docID = kDocID;
    rq.body = kFleeceBody;
    rq.save = true;
    auto doc = c4doc_put(db, &rq, nullptr, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    C4Slice kExpectedRevID;
	if(isRevTrees()) {
		kExpectedRevID = C4STR("1-042ca1d3a1d16fd5ab2f87efc7ebbf50b7498032");
	} else {
        kExpectedRevID = C4STR("1@*");
	}

    CHECK(doc->revID == kExpectedRevID);
    CHECK(doc->flags == kDocExists);
    CHECK(doc->selectedRev.revID == kExpectedRevID);
    c4doc_release(doc);

    // Update doc:
    auto body = json2fleece("{'ok':'go'}");
    rq.body = body;
    rq.history = &kExpectedRevID;
    rq.historyCount = 1;
    size_t commonAncestorIndex;
    doc = c4doc_put(db, &rq, &commonAncestorIndex, &error);
    REQUIRE(doc != nullptr);
    CHECK((unsigned long)commonAncestorIndex == 0ul);
    C4Slice kExpectedRev2ID;
	if(isRevTrees()) {
		kExpectedRev2ID = C4STR("2-201796aeeaa6ddbb746d6cab141440f23412ac51");
	} else {
        kExpectedRev2ID = C4STR("2@*");
	}

    CHECK(doc->revID == kExpectedRev2ID);
    CHECK(doc->flags == kDocExists);
    CHECK(doc->selectedRev.revID == kExpectedRev2ID);
    c4doc_release(doc);

    // Insert existing rev that conflicts:
    body = json2fleece("{'from':'elsewhere'}");
    rq.body = body;
    rq.existingRevision = true;
    rq.remoteDBID = 1;
    C4Slice kConflictRevID;
	if(isRevTrees()) {
		kConflictRevID = C4STR("2-deadbeef");
	} else {
        kConflictRevID = C4STR("1@binky");
	}

    C4Slice history[2] = {kConflictRevID, kExpectedRevID};
    rq.history = history;
    rq.historyCount = 2;
    rq.allowConflict = true;
    doc = c4doc_put(db, &rq, &commonAncestorIndex, &error);
    REQUIRE(doc != nullptr);
    CHECK((unsigned long)commonAncestorIndex == 1ul);
    CHECK(doc->selectedRev.revID == kConflictRevID);
    CHECK(doc->flags == (kDocExists | kDocConflicted));
    // The conflicting rev will now never be the default, even with rev-trees.
    CHECK(doc->revID == kExpectedRev2ID);
    
    auto latestBody = c4doc_detachRevisionBody(doc);
    CHECK(latestBody == rq.body);
    CHECK(latestBody.buf != doc->selectedRev.body.buf);
    c4doc_release(doc);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Update", "[Database][C]") {
    C4Log("Begin test");
    C4Error error;
    C4Document *doc;

    {
        C4Log("Begin create");
        TransactionHelper t(db);
        doc = c4doc_create(db, kDocID, kFleeceBody, 0, &error);
        REQUIRE(doc);
    }
    C4Log("After save");
    C4Slice kExpectedRevID;
	if(isRevTrees()) {
		kExpectedRevID = C4STR("1-042ca1d3a1d16fd5ab2f87efc7ebbf50b7498032");
	} else {
        kExpectedRevID = C4STR("1@*");
	}

    CHECK(doc->revID == kExpectedRevID);
    CHECK(doc->flags == kDocExists);
    CHECK(doc->selectedRev.revID == kExpectedRevID);
    CHECK(doc->docID == kDocID);

    // Read the doc into another C4Document:
    auto doc2 = c4doc_get(db, kDocID, false, &error);
    REQUIRE(doc2->revID == kExpectedRevID);

    // Update it a few times:
    for (int update = 2; update <= 5; ++update) {
        C4Log("Begin save #%d", update);
        TransactionHelper t(db);
        fleece::alloc_slice oldRevID(doc->revID);
        auto updatedDoc = c4doc_update(doc, json2fleece("{'ok':'go'}"), 0, &error);
        REQUIRE(updatedDoc);
        CHECK(doc->selectedRev.revID == oldRevID);
        CHECK(doc->revID == oldRevID);
        c4doc_release(doc);
        doc = updatedDoc;
    }
    C4Log("After multiple updates");
	C4Slice kExpectedRev2ID;
	if(isRevTrees()) {
		kExpectedRev2ID = C4STR("5-a452899fa8e69b06d936a5034018f6fff0a8f906");
	} else {
        kExpectedRev2ID = C4STR("5@*");
	}

    CHECK(doc->revID == kExpectedRev2ID);
    CHECK(doc->selectedRev.revID == kExpectedRev2ID);

    // Now try to update the other C4Document, which will fail:
    {
        C4Log("Begin conflicting save");
        TransactionHelper t(db);
        REQUIRE(c4doc_update(doc2, json2fleece("{'ok':'no way'}"), 0, &error) == nullptr);
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorConflict);
    }

    // Try to create a new doc with the same ID, which will fail:
    {
        C4Log("Begin conflicting create");
        TransactionHelper t(db);
        REQUIRE(c4doc_create(db, kDocID, json2fleece("{'ok':'no way'}"), 0, &error) == nullptr);
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorConflict);
    }

    c4doc_release(doc);
    c4doc_release(doc2);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Conflict", "[Database][C]") {
    if (!isRevTrees())
        return;

    const auto kFleeceBody2 = json2fleece("{'ok':'go'}");
    const auto kFleeceBody3 = json2fleece("{'ubu':'roi'}");
    createRev(kDocID, kRevID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody2, kRevKeepBody);
    createRev(kDocID, C4STR("3-aaaaaa"), kFleeceBody3);

    TransactionHelper t(db);

    // "Pull" a conflicting revision:
    C4Slice history[3] = {C4STR("4-dddd"), C4STR("3-ababab"), kRev2ID};
    C4DocPutRequest rq = {};
    rq.existingRevision = true;
    rq.docID = kDocID;
    rq.history = history;
    rq.historyCount = 3;
    rq.allowConflict = true;
    rq.body = kFleeceBody3;
    rq.save = true;
    rq.remoteDBID = 1;
    C4Error err;
    auto doc = c4doc_put(db, &rq, nullptr, &err);
    REQUIRE(doc);

    // Check that the pulled revision is treated as a conflict:
	C4Slice revID = C4STR("4-dddd");
    CHECK(doc->selectedRev.revID == revID);
    CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevIsConflict));
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK((int)doc->selectedRev.flags == kRevIsConflict);

    // Check that the local revision is still current:
	revID = C4STR("3-aaaaaa");
    CHECK(doc->revID == revID);
    REQUIRE(c4doc_selectCurrentRevision(doc));
    CHECK(doc->selectedRev.revID == revID);
    CHECK((int)doc->selectedRev.flags == kRevLeaf);

    // Now check the common ancestor algorithm:
    REQUIRE(c4doc_selectCommonAncestorRevision(doc, C4STR("3-aaaaaa"), C4STR("4-dddd")));
    CHECK(doc->selectedRev.revID == kRev2ID);

    REQUIRE(c4doc_selectCommonAncestorRevision(doc, C4STR("4-dddd"), C4STR("3-aaaaaa")));
    CHECK(doc->selectedRev.revID == kRev2ID);

    REQUIRE(c4doc_selectCommonAncestorRevision(doc, C4STR("3-ababab"), C4STR("3-aaaaaa")));
    CHECK(doc->selectedRev.revID == kRev2ID);
    REQUIRE(c4doc_selectCommonAncestorRevision(doc, C4STR("3-aaaaaa"), C4STR("3-ababab")));
    CHECK(doc->selectedRev.revID == kRev2ID);

    REQUIRE(c4doc_selectCommonAncestorRevision(doc, kRev2ID, C4STR("3-aaaaaa")));
    CHECK(doc->selectedRev.revID == kRev2ID);
    REQUIRE(c4doc_selectCommonAncestorRevision(doc, C4STR("3-aaaaaa"), kRev2ID));
    CHECK(doc->selectedRev.revID == kRev2ID);

    REQUIRE(c4doc_selectCommonAncestorRevision(doc, kRev2ID, kRev2ID));
    CHECK(doc->selectedRev.revID == kRev2ID);

    auto mergedBody = json2fleece("{\"merged\":true}");

    SECTION("Merge, 4 wins") {
        REQUIRE(c4doc_resolveConflict(doc, C4STR("4-dddd"), C4STR("3-aaaaaa"),
                                      mergedBody, 0, &err));
        c4doc_selectCurrentRevision(doc);
		revID = C4STR("5-79b2ecd897d65887a18c46cc39db6f0a3f7b38c4");
        CHECK(doc->selectedRev.revID == revID);
        CHECK(doc->selectedRev.body == mergedBody);
        CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevNew));
        c4doc_selectParentRevision(doc);
		revID = C4STR("4-dddd");
        CHECK(doc->selectedRev.revID == revID);
        CHECK((int)doc->selectedRev.flags == 0);
        c4doc_selectParentRevision(doc);
		revID = C4STR("3-ababab");
        CHECK(doc->selectedRev.revID == revID);
        CHECK((int)doc->selectedRev.flags == 0);
    }

    SECTION("Merge, 3 wins") {
        REQUIRE(c4doc_resolveConflict(doc, C4STR("3-aaaaaa"), C4STR("4-dddd"),
                                      mergedBody, 0, &err));
        c4doc_selectCurrentRevision(doc);
		revID = C4STR("4-1fa2dbcb66b5e0456f6d6fc4a90918d42f3dd302");
        CHECK(doc->selectedRev.revID == revID);
        CHECK(doc->selectedRev.body == mergedBody);
        CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevNew));
        c4doc_selectParentRevision(doc);
		revID = C4STR("3-aaaaaa");
        CHECK(doc->selectedRev.revID == revID);
        CHECK((int)doc->selectedRev.flags == 0);
    }

    c4doc_release(doc);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document from Fleece", "[Database][C]") {
    if (!isRevTrees())
        return;

    CHECK(c4doc_containingValue((FLValue)0x12345678) == nullptr);

    const auto kFleeceBody = json2fleece("{'ubu':'roi'}");
    createRev(kDocID, kRevID, kFleeceBody);

    C4Document* doc = c4doc_get(db, kDocID, true, nullptr);
    REQUIRE(doc);
    FLValue root = FLValue_FromData(doc->selectedRev.body, kFLTrusted);
    REQUIRE(root);
    CHECK(c4doc_containingValue(root) == doc);
    FLValue ubu = FLDict_Get(FLValue_AsDict(root), "ubu"_sl);
    CHECK(c4doc_containingValue(ubu) == doc);
    c4doc_release(doc);

    CHECK(c4doc_containingValue(root) == nullptr);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Leaf Document from Fleece", "[Database][C]") {
    if (!isRevTrees())
        return;

    CHECK(c4doc_containingValue((FLValue)0x12345678) == nullptr);

    const auto kFleeceBody = json2fleece("{'ubu':'roi'}");
    createRev(kDocID, kRevID, kFleeceBody);

    C4Document* doc = c4doc_getSingleRevision(db, kDocID, nullslice, true, nullptr);
    REQUIRE(doc);
    CHECK(doc->selectedRev.revID == kRevID);
    FLValue root = FLValue_FromData(doc->selectedRev.body, kFLTrusted);
    REQUIRE(root);
    CHECK(c4doc_containingValue(root) == doc);
    FLValue ubu = FLDict_Get(FLValue_AsDict(root), "ubu"_sl);
    CHECK(c4doc_containingValue(ubu) == doc);
    c4doc_release(doc);

    CHECK(c4doc_containingValue(root) == nullptr);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties", "[Database][C]") {
    CHECK(c4doc_isOldMetaProperty(C4STR("_attachments")));
    CHECK(!c4doc_isOldMetaProperty(C4STR("@type")));
    
    FLEncoder enc = c4db_getSharedFleeceEncoder(db);
    
    {
        TransactionHelper t(db);
        FLEncoder_BeginDict(enc, 2);
        FLEncoder_WriteKey(enc, FLSTR("@type"));
        FLEncoder_WriteString(enc, FLSTR("blob"));
        FLEncoder_WriteKey(enc, FLSTR("digest"));
        FLEncoder_WriteString(enc, FLSTR(""));
        FLEncoder_EndDict(enc);
    }
    
    FLDoc result = FLEncoder_FinishDoc(enc, nullptr);
    REQUIRE(result);
    REQUIRE(FLDoc_GetSharedKeys(result));
    FLValue val = FLDoc_GetRoot(result);
    FLDict d = FLValue_AsDict(val);
    REQUIRE(d);
    
    FLDictKey testKey = FLDictKey_Init(C4STR("@type"));
    FLValue testVal = FLDict_GetWithKey(d, &testKey);
    
    FLSlice blobSl = FLSTR("blob"); // Windows cannot compile this inside of a REQUIRE
    REQUIRE(FLValue_AsString(testVal) == blobSl);

    REQUIRE(FLValue_FindDoc((FLValue)d) == result);
    CHECK(c4doc_dictContainsBlobs(d));
    FLDoc_Release(result);
    
    enc = c4db_getSharedFleeceEncoder(db);
    FLEncoder_BeginDict(enc, 0);
    FLEncoder_EndDict(enc);
    result = FLEncoder_FinishDoc(enc, nullptr);
    REQUIRE(result);
    val = val = FLDoc_GetRoot(result);
    d = FLValue_AsDict(val);
    REQUIRE(d);
    
    CHECK(!c4doc_dictContainsBlobs(d));
    FLDoc_Release(result);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties 2", "[Database][C]") {
    // Check that old meta properties get removed:
    TransactionHelper t(db);
    auto sk = c4db_getFLSharedKeys(db);
    auto dict = json2dict("{_id:'foo', _rev:'1-2345', x:17}");
    CHECK(c4doc_hasOldMetaProperties(dict));
    alloc_slice stripped = c4doc_encodeStrippingOldMetaProperties(dict, sk, NULL);
    Doc doc(stripped, kFLTrusted, sk);
    CHECK(fleece2json(stripped) == "{x:17}");
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties 3", "[Database][C]") {
    // Check that _attachments isn't removed if there are non-translated attachments in it,
    // but that the translated-from-blob attachments are removed:
    TransactionHelper t(db);
    auto sk = c4db_getFLSharedKeys(db);
    auto dict = json2dict("{_attachments: {'blob_/foo/1': {'digest': 'sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU='},"
                                           "oldie: {'digest': 'sha1-xVVVVVVVVVVVVVVVVVVVVVVVVVU='} },"
                           "foo: [ 0, {'@type':'blob', digest:'sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU='} ] }");
    CHECK(c4doc_hasOldMetaProperties(dict));
    alloc_slice stripped = c4doc_encodeStrippingOldMetaProperties(dict, sk, NULL);
    Doc doc(stripped, kFLTrusted, sk);
    CHECK(fleece2json(stripped) == "{_attachments:{oldie:{digest:\"sha1-xVVVVVVVVVVVVVVVVVVVVVVVVVU=\"}},foo:[0,{\"@type\":\"blob\",digest:\"sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU=\"}]}");
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties 4", "[Database][C]") {
    // Check that a translated attachment whose digest is different than its blob (i.e. the
    // attachment was probably modified by a non-blob-aware system) has its digest transferred to
    // the blob before being deleted. See #507. (Also, the _attachments property should be deleted.)
    TransactionHelper t(db);
    auto sk = c4db_getFLSharedKeys(db);
    auto dict = json2dict("{_attachments: {'blob_/foo/1': {'digest': 'sha1-XXXVVVVVVVVVVVVVVVVVVVVVVVU=',content_type:'image/png',revpos:23}},"
                           "foo: [ 0, {'@type':'blob', digest:'sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU=',content_type:'text/plain'} ] }");
    CHECK(c4doc_hasOldMetaProperties(dict));
    alloc_slice stripped = c4doc_encodeStrippingOldMetaProperties(dict, sk, NULL);
    Doc doc(stripped, kFLTrusted, sk);
    CHECK(fleece2json(stripped) == "{foo:[0,{\"@type\":\"blob\",content_type:\"image/png\",digest:\"sha1-XXXVVVVVVVVVVVVVVVVVVVVVVVU=\"}]}");
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties 5", "[Database][C]") {
    // Check that the 2.0.0 blob_<number> gets removed:
    TransactionHelper t(db);
    auto sk = c4db_getFLSharedKeys(db);
    auto dict = json2dict("{_attachments: {'blob_1': {'digest': 'sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU=',content_type:'image/png',revpos:23}},"
                           "foo: [ 0, {'@type':'blob', digest:'sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU=',content_type:'text/plain'} ] }");
    CHECK(c4doc_hasOldMetaProperties(dict));
    alloc_slice stripped = c4doc_encodeStrippingOldMetaProperties(dict, sk, NULL);
    Doc doc(stripped, kFLTrusted, sk);
    CHECK(fleece2json(stripped) == "{foo:[0,{\"@type\":\"blob\",content_type:\"text/plain\",digest:\"sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU=\"}]}");
}


// Repro case for https://github.com/couchbase/couchbase-lite-core/issues/478
N_WAY_TEST_CASE_METHOD(C4Test, "Document Clobber Remote Rev", "[Database][C]") {

    if (!isRevTrees())
        return;

    TransactionHelper t(db);

    // Write doc to db
    createRev(kDocID, kRevID, kFleeceBody);

    // Use default remote id
    C4RemoteID testRemoteId = 1;

    // Read doc from db and keep in memory
    C4Error error;
    auto curDoc = c4doc_get(db, kDocID, false, &error);
    REQUIRE(curDoc != nullptr);

    // Call MarkRevSynced which will set the flag
    bool markSynced = c4db_markSynced(db, kDocID, curDoc->sequence, testRemoteId, &error);
    REQUIRE(markSynced);

    // Get the latest version of the doc
    auto curDocAfterMarkSync = c4doc_get(db, kDocID, false, &error);
    REQUIRE(curDocAfterMarkSync != nullptr);

    // Get the remote ancesor rev, and make sure it matches up with the latest rev of the doc
    FLSliceResult remoteRevID = c4doc_getRemoteAncestor(curDocAfterMarkSync, testRemoteId);
    REQUIRE(remoteRevID == curDocAfterMarkSync->revID);

    // Update doc -- before the bugfix, this was clobbering the remote ancestor rev
    auto updatedDoc = c4doc_update(curDoc, json2fleece("{'ok':'go'}"), 0, &error);
    REQUIRE(updatedDoc);

    // Re-read the doc from the db just to be sure getting accurate version
    auto updatedDocRefreshed = c4doc_get(db, kDocID, false, &error);

    // Check the remote ancestor rev of the updated doc and make sure it has not been clobbered.
    // Before the bug fix for LiteCore #478, this was returning an empty value.
    FLSliceResult remoteRevIDAfterupdate = c4doc_getRemoteAncestor(updatedDocRefreshed, testRemoteId);
    REQUIRE(remoteRevIDAfterupdate == curDocAfterMarkSync->revID);

    // Cleanup
    c4doc_release(curDoc);
    c4doc_release(curDocAfterMarkSync);
    c4doc_release(updatedDoc);
    c4doc_release(updatedDocRefreshed);
}

