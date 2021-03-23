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


N_WAY_TEST_CASE_METHOD(C4Test, "Invalid docID", "[Document][C]") {
    c4log_warnOnErrors(false);
    TransactionHelper t(db);

    auto checkPutBadDocID = [this](C4Slice docID) {
        C4Error error;
        C4DocPutRequest rq = {};
        rq.body = C4Test::kFleeceBody;
        rq.save = true;
        rq.docID = docID;
        ExpectingExceptions x;
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


#if 0
N_WAY_TEST_CASE_METHOD(C4Test, "Document PossibleAncestors", "[Document][C]") {
    if (!isRevTrees()) return;

    createRev(kDocID, kRevID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody);
    createRev(kDocID, kRev3ID, kFleeceBody);

    C4Document *doc = c4doc_get(db, kDocID, true, ERROR_INFO());
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
#endif


N_WAY_TEST_CASE_METHOD(C4Test, "Document FindDocAncestors", "[Document][C]") {
    C4String doc1 = C4STR("doc1"), doc2 = C4STR("doc2"), doc3 = C4STR("doc3");
    auto toString = [](C4SliceResult sr) {return std::string(alloc_slice(std::move(sr)));};
    static constexpr bool kNoBodies = false;
    C4RemoteID kRemoteID = 1;
    static constexpr unsigned kMaxAncestors = 4;
    C4SliceResult ancestors[4] = {};

    auto findDocAncestor = [&](C4Slice docID, C4Slice revID, bool requireBodies = false) -> std::string {
        C4SliceResult ancestors[1] = {};
        REQUIRE(c4db_findDocAncestors(db, 1, kMaxAncestors, requireBodies, kRemoteID, &docID, &revID, ancestors, WITH_ERROR()));
        return toString(std::move(ancestors[0]));
    };

    if (isRevTrees()) {
        // Rev-trees:
        createRev(doc1, kRevID, kFleeceBody);
        createRev(doc1, kRev2ID, kFleeceBody);
        createRev(doc1, kRev3ID, kFleeceBody);

        createRev(doc2, kRevID, kFleeceBody);
        createRev(doc2, kRev2ID, kFleeceBody);
        createRev(doc2, kRev3ID, kFleeceBody);

        createRev(doc3, kRevID, kFleeceBody);
        createRev(doc3, kRev2ID, kFleeceBody);
        createRev(doc3, kRev3ID, kFleeceBody);

        // Doc I don't have yet:
        CHECK(findDocAncestor("new"_sl, kRev3ID) == "");

        // Revision I already have:
        CHECK(findDocAncestor(doc1, kRev3ID) == "8"); // kRevHaveLocal | kRevSame

        // Newer revision:
        CHECK(findDocAncestor(doc1, "4-deadbeef"_sl) == R"(1["3-deadbeef","2-c001d00d","1-abcd"])");

        // Require bodies:
        CHECK(findDocAncestor(doc1, "4-deadbeef"_sl, true) == R"(1["3-deadbeef"])");

        // Conflict:
        CHECK(findDocAncestor(doc1, "3-00000000"_sl) == R"(3["2-c001d00d","1-abcd"])");

        // Limit number of results:
        C4Slice newRevID = "4-deadbeef"_sl;
        REQUIRE(c4db_findDocAncestors(db, 1, 1, kNoBodies, kRemoteID, &doc1, &newRevID, ancestors, WITH_ERROR()));
        CHECK(toString(std::move(ancestors[0])) == R"(1["3-deadbeef"])");

        // Multiple docs:
        C4String docIDs[4] = {doc2,            doc1,    C4STR("doc4"),    doc3};
        C4String revIDs[4] = {"4-deadbeef"_sl, kRev3ID, C4STR("17-eeee"), "2-f000"_sl};
        REQUIRE(c4db_findDocAncestors(db, 4, kMaxAncestors, kNoBodies, kRemoteID, docIDs, revIDs, ancestors, WITH_ERROR()));
        CHECK(toString(std::move(ancestors[0])) == R"(1["3-deadbeef","2-c001d00d","1-abcd"])");
        CHECK(toString(std::move(ancestors[1])) == "8");
        CHECK(!slice(ancestors[2]));
        CHECK(toString(std::move(ancestors[3])) == R"(3["1-abcd"])");

    } else {
        // Version-vectors:
        createRev(doc1, "3@100,10@8"_sl, kFleeceBody);
        createRev(doc2, "3@100,10@8"_sl, kFleeceBody);
        createRev(doc3, "3@300,30@8"_sl, kFleeceBody);

        // Doc I don't have yet:
        CHECK(findDocAncestor("new"_sl, "3@300,30@8"_sl) == "");

        // Revision I already have:
        CHECK(findDocAncestor(doc1, "3@100,10@8"_sl) == "0");// kRevSame

        // Require bodies:
        CHECK(findDocAncestor(doc1, "3@100,10@8"_sl, true) == "0");// kRevSame

        // Older revision:
        CHECK(findDocAncestor(doc1, "2@100,10@8"_sl) == "2");// kRevNewer

        // Require bodies:
        CHECK(findDocAncestor(doc1, "2@100,10@8"_sl, true) == "2");

        // Newer revision:
        CHECK(findDocAncestor(doc1, "11@8,3@100"_sl) == R"(1["3@100,10@8"])");

        // Conflict:
        CHECK(findDocAncestor(doc1, "4@100,9@8"_sl) == R"(3[])");

        // Single version:
        CHECK(findDocAncestor(doc1, "10@8"_sl) == "2");
        CHECK(findDocAncestor(doc1, "11@8"_sl) == "3[]");
        CHECK(findDocAncestor(doc1, "1@99"_sl) == "3[]");

        // Limit number of results:
        C4Slice newRevID = "11@8,3@100"_sl;
        REQUIRE(c4db_findDocAncestors(db, 1, 1, kNoBodies, kRemoteID, &doc1, &newRevID, ancestors, WITH_ERROR()));
        CHECK(toString(std::move(ancestors[0])) == R"(1["3@100,10@8"])");

        // Multiple docs:
        C4String docIDs[4] = {doc2,            doc1,            C4STR("doc4"),    doc3};
        C4String revIDs[4] = {"9@100,10@8"_sl, "3@100,10@8"_sl, C4STR("17@17"),   "1@99,3@300,30@8"_sl};
        REQUIRE(c4db_findDocAncestors(db, 4, kMaxAncestors, kNoBodies, kRemoteID, docIDs, revIDs, ancestors, WITH_ERROR()));
        CHECK(toString(std::move(ancestors[0])) == R"(1["3@100,10@8"])");
        CHECK(alloc_slice(std::move(ancestors[1])) == "0");
        CHECK(!slice(ancestors[2]));
        CHECK(toString(std::move(ancestors[3])) == R"(1["3@300,30@8"])");
    }
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document CreateVersionedDoc", "[Document][C]") {
    // Try reading doc with mustExist=true, which should fail:
    C4Error error;
    C4Document* doc;
    doc = c4doc_get(db, kDocID, true, &error);
    REQUIRE(!doc);
    REQUIRE((uint32_t)error.domain == (uint32_t)LiteCoreDomain);
    REQUIRE(error.code == (int)kC4ErrorNotFound);
    c4doc_release(doc);

    // Test c4db_getDoc, which also fails:
    for (C4DocContentLevel content : {kDocGetMetadata, kDocGetCurrentRev, kDocGetAll}) {
        doc = c4db_getDoc(db, kDocID, true, content, &error);
        REQUIRE(!doc);
        REQUIRE((uint32_t)error.domain == (uint32_t)LiteCoreDomain);
        REQUIRE(error.code == (int)kC4ErrorNotFound);
    }

    // Now get the doc with mustExist=false, which returns an empty doc:
    doc = c4doc_get(db, kDocID, false, ERROR_INFO(error));
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
        doc = c4doc_put(db, &rq, nullptr, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        CHECK(doc->revID == kRevID);
        CHECK(doc->selectedRev.revID == kRevID);
        if (isRevTrees())
            CHECK(doc->selectedRev.flags == (kRevKeepBody | kRevLeaf));
        else
            CHECK(doc->selectedRev.flags == (kRevLeaf));
        CHECK(docBodyEquals(doc, kFleeceBody));
        c4doc_release(doc);
    }

    // Reload the doc:
    doc = c4doc_get(db, kDocID, true, ERROR_INFO(error));
    REQUIRE(doc != nullptr);
    CHECK(doc->sequence == 1);
    CHECK(doc->flags == kDocExists);
    CHECK(doc->docID == kDocID);
    CHECK(doc->revID == kRevID);
    CHECK(doc->selectedRev.revID == kRevID);
    CHECK(doc->selectedRev.sequence == 1);
    CHECK(docBodyEquals(doc, kFleeceBody));
    c4doc_release(doc);

    // Get the doc by its sequence:
    doc = c4doc_getBySequence(db, 1, ERROR_INFO(error));
    REQUIRE(doc != nullptr);
    CHECK(doc->sequence == 1);
    CHECK(doc->flags == kDocExists);
    CHECK(doc->docID == kDocID);
    CHECK(doc->revID == kRevID);
    CHECK(doc->selectedRev.revID == kRevID);
    CHECK(doc->selectedRev.sequence == 1);
    CHECK(docBodyEquals(doc, kFleeceBody));
    if (isRevTrees()) {
        {
            TransactionHelper t(db);
            CHECK(c4doc_removeRevisionBody(doc));
            CHECK(c4doc_selectCurrentRevision(doc));
        }
        CHECK(c4doc_getProperties(doc) == nullptr);
    }
    c4doc_release(doc);

    // Get a bogus sequence
    doc = c4doc_getBySequence(db, 2, &error);
    CHECK(doc == nullptr);
    CHECK(error == C4Error{LiteCoreDomain, kC4ErrorNotFound});

    // Test c4db_getDoc:
    for (C4DocContentLevel content : {kDocGetMetadata, kDocGetCurrentRev, kDocGetAll}) {
        doc = c4db_getDoc(db, kDocID, true, content, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        CHECK(doc->sequence == 1);
        CHECK(doc->flags == kDocExists);
        CHECK(doc->docID == kDocID);
        CHECK(doc->revID == kRevID);
        CHECK(doc->selectedRev.revID == kRevID);
        CHECK(doc->selectedRev.sequence == 1);
        if (content == kDocGetMetadata)
            CHECK(c4doc_getProperties(doc) == nullptr);
        else
            CHECK(docBodyEquals(doc, kFleeceBody));
        c4doc_release(doc);
    }
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document CreateMultipleRevisions", "[Document][C]") {
    const auto kFleeceBody2 = json2fleece("{'ok':'go'}");
    const auto kFleeceBody3 = json2fleece("{'ubu':'roi'}");
    createRev(kDocID, kRevID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody2, kRevKeepBody);
    createRev(kDocID, kRev2ID, kFleeceBody2); // test redundant insert

    // Reload the doc:
    C4Error error;
    C4Document *doc = c4db_getDoc(db, kDocID, true, kDocGetAll, ERROR_INFO(error));
    REQUIRE(doc != nullptr);
    CHECK(doc->flags == kDocExists);
    CHECK(doc->docID == kDocID);
    CHECK(doc->revID == kRev2ID);
    CHECK(doc->selectedRev.revID == kRev2ID);
    CHECK(doc->selectedRev.sequence == (C4SequenceNumber)2);
    CHECK(docBodyEquals(doc, kFleeceBody2));

    if (isRevTrees()) {
        alloc_slice history = c4doc_getRevisionHistory(doc, 99, nullptr, 0);
        CHECK(history == "2-c001d00d,1-abcd");

        // Select 1st revision:
        REQUIRE(c4doc_selectParentRevision(doc));
        CHECK(doc->selectedRev.revID == kRevID);
        CHECK(doc->selectedRev.sequence == (C4SequenceNumber)1);
        CHECK(c4doc_getProperties(doc) == nullptr);
        CHECK(!c4doc_hasRevisionBody(doc));
        CHECK(!c4doc_selectParentRevision(doc));
        c4doc_release(doc);

        // Add a 3rd revision:
        createRev(kDocID, kRev3ID, kFleeceBody3);
        // Revision 2 should keep its body due to the kRevKeepBody flag:
        doc = c4db_getDoc(db, kDocID, true, kDocGetAll, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        REQUIRE(c4doc_selectParentRevision(doc));
        CHECK(doc->selectedRev.revID == kRev2ID);
        CHECK(doc->selectedRev.sequence == (C4SequenceNumber)2);
        CHECK(doc->selectedRev.flags == kRevKeepBody);
        CHECK(docBodyEquals(doc, kFleeceBody2));
        c4doc_release(doc);

        // Test c4db_getDoc:
        for (C4DocContentLevel content : {kDocGetMetadata, kDocGetCurrentRev, kDocGetAll}) {
            doc = c4db_getDoc(db, kDocID, true, content, ERROR_INFO(error));
            REQUIRE(doc != nullptr);
            CHECK(doc->sequence == 3);
            CHECK(doc->flags == kDocExists);
            CHECK(doc->docID == kDocID);
            CHECK(doc->revID == kRev3ID);
            CHECK(doc->selectedRev.revID == kRev3ID);
            CHECK(doc->selectedRev.sequence == 3);
            if (content == kDocGetMetadata)
                CHECK(c4doc_getProperties(doc) == nullptr);
            else
                CHECK(docBodyEquals(doc, kFleeceBody3));
            c4doc_release(doc);
        }

        // Purge doc
        {
            TransactionHelper t(db);
            doc = c4doc_get(db, kDocID, true, ERROR_INFO(error));
            REQUIRE(doc);
            int nPurged = c4doc_purgeRevision(doc, {}, ERROR_INFO(error));
            CHECK(nPurged == 3);
            REQUIRE(c4doc_save(doc, 20, WITH_ERROR(&error)));
            c4doc_release(doc);
        }

        // Make sure it's gone:
        doc = c4doc_get(db, kDocID, true, &error);
        CHECK(!doc);
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorNotFound);
    } else {
        // The history is going to end with this database's peerID, a random 64-bit hex string,
        // so we don't know exactly what it will be. But it will start "2@".
        alloc_slice history = c4doc_getRevisionHistory(doc, 99, nullptr, 0);
        CHECK(history.hasPrefix("2@"));
        CHECK(history.size <= 2 + 32);
    }
    c4doc_release(doc);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Purge", "[Database][Document][C]") {
    C4Error err;
    const auto kFleeceBody2 = json2fleece("{'ok':'go'}");
    const auto kFleeceBody3 = json2fleece("{'ubu':'roi'}");
    createRev(kDocID, kRevID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody2);
    createRev(kDocID, kRev3ID, kFleeceBody3);

    C4DocPutRequest rq = {};
    C4Slice history[3] = {C4STR("3-ababab"), kRev2ID};
    if (isRevTrees()) {
        // Create a conflict
        rq.existingRevision = true;
        rq.docID = kDocID;
        rq.history = history;
        rq.historyCount = 2;
        rq.allowConflict = true;
        rq.body = kFleeceBody3;
        rq.save = true;
        REQUIRE(c4db_beginTransaction(db, WITH_ERROR(&err)));
        auto doc = c4doc_put(db, &rq, nullptr, ERROR_INFO(err));
        REQUIRE(doc);
        c4doc_release(doc);
        REQUIRE(c4db_endTransaction(db, true, WITH_ERROR(&err)));
    }

    REQUIRE(c4db_beginTransaction(db, WITH_ERROR(&err)));
    REQUIRE(c4db_purgeDoc(db, kDocID, WITH_ERROR(&err)));
    REQUIRE(c4db_endTransaction(db, true, WITH_ERROR(&err)));
    
    REQUIRE(c4db_getDocumentCount(db) == 0);
    
    if (isRevTrees()) {
        // c4doc_purgeRevision is not available with version vectors
        createRev(kDocID, kRevID, kFleeceBody);
        createRev(kDocID, kRev2ID, kFleeceBody2);
        createRev(kDocID, kRev3ID, kFleeceBody3);
        REQUIRE(c4db_beginTransaction(db, WITH_ERROR(&err)));
        auto doc = c4doc_put(db, &rq, nullptr, ERROR_INFO(err));
        REQUIRE(doc);
        REQUIRE(c4db_endTransaction(db, true, WITH_ERROR(&err)));
        c4doc_release(doc);

        REQUIRE(c4db_beginTransaction(db, WITH_ERROR(&err)));
        doc = c4doc_get(db, kDocID, true, ERROR_INFO(err));
        REQUIRE(doc);
        CHECK(c4doc_purgeRevision(doc, kRev2ID, WITH_ERROR(&err)) == 0);
        REQUIRE(c4doc_purgeRevision(doc, kC4SliceNull, WITH_ERROR(&err)) == 4);
        REQUIRE(c4doc_save(doc, 20, WITH_ERROR(&err)));
        c4doc_release(doc);
        REQUIRE(c4db_endTransaction(db, true, WITH_ERROR(&err)));
        REQUIRE(c4db_getDocumentCount(db) == 0);
    }
}


#if 0
N_WAY_TEST_CASE_METHOD(C4Test, "Document maxRevTreeDepth", "[Database][Document][C]") {
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

        auto doc = c4doc_get(db, docID, false, ERROR_INFO(error));
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
                auto savedDoc = c4doc_put(db, &rq, nullptr, ERROR_INFO(error));
                REQUIRE(savedDoc != nullptr);
                c4doc_release(doc);
                doc = savedDoc;
            }
        }
        C4Log("Created %u revisions in %.3f sec", kNumRevs, st.elapsed());

        if (isRevTrees()) {
            // Check rev tree depth:
            unsigned nRevs = 0;
            c4doc_selectCurrentRevision(doc);
            do {
                unsigned expectedGen = kNumRevs - nRevs;
                if (setRemoteOrigin && nRevs == 30)
                    expectedGen = 1; // the remote-origin rev is pinned
                CHECK(c4rev_getGeneration(doc->selectedRev.revID) == expectedGen);
                ++nRevs;
            } while (c4doc_selectParentRevision(doc));
            C4Log("Document rev tree depth is %u", nRevs);
            REQUIRE(nRevs == (setRemoteOrigin ? 31 : 30));
        }
        c4doc_release(doc);
    }
}
#endif


#if 0
N_WAY_TEST_CASE_METHOD(C4Test, "Document GetForPut", "[Document][C]") {
    C4Error error;
    TransactionHelper t(db);

    // Creating doc given ID:
    auto doc = c4doc_getForPut(db, kDocID, kC4SliceNull, false, false, ERROR_INFO(error));
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kC4SliceNull);
    REQUIRE(doc->flags == 0);
    REQUIRE(doc->selectedRev.revID == kC4SliceNull);
    c4doc_release(doc);

    // Creating doc, no ID:
    doc = c4doc_getForPut(db, kC4SliceNull, kC4SliceNull, false, false, ERROR_INFO(error));
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
    doc = c4doc_getForPut(db, kDocID, kRevID, false, false, ERROR_INFO(error));
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
        doc = c4doc_getForPut(db, kDocID, kRevID, false, true/*allowConflicts*/, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        REQUIRE(doc->docID == kDocID);
        REQUIRE(doc->selectedRev.revID == kRevID);
        c4doc_release(doc);
    }

    // Deleting the doc:
    doc = c4doc_getForPut(db, kDocID, kRev2ID, true/*deleted*/, false, ERROR_INFO(error));
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->selectedRev.revID == kRev2ID);
    c4doc_release(doc);
    
    // Actually delete it:
    createRev(kDocID, kRev3ID, kC4SliceNull, kRevDeleted);

    // Re-creating the doc (no revID given):
    doc = c4doc_getForPut(db, kDocID, kC4SliceNull, false, false, ERROR_INFO(error));
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRev3ID);
    REQUIRE(doc->flags == (kDocExists | kDocDeleted));
    REQUIRE(doc->selectedRev.revID == kRev3ID);
    c4doc_release(doc);
}
#endif


N_WAY_TEST_CASE_METHOD(C4Test, "Document Put", "[Document][C]") {
    C4Error error;
    C4Document *doc = nullptr;
    size_t commonAncestorIndex;
    alloc_slice revID;

    TransactionHelper t(db);

    C4Slice kExpectedRevID, kExpectedRev2ID, kConflictRevID;
    if(isRevTrees()) {
        kExpectedRevID = C4STR("1-042ca1d3a1d16fd5ab2f87efc7ebbf50b7498032");
    } else {
        kExpectedRevID = C4STR("1@*");
    }

    C4DocPutRequest rqTemplate = {};
    rqTemplate.docID = kDocID;
    rqTemplate.body = kFleeceBody;
    rqTemplate.save = true;

    // Creating doc given ID:
    {
        C4DocPutRequest rq = rqTemplate;
        rq.docID = kDocID;
        rq.body = kFleeceBody;
        rq.save = true;
        doc = c4doc_put(db, &rq, nullptr, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        REQUIRE(doc->docID == kDocID);

        CHECK(doc->revID == kExpectedRevID);
        CHECK(doc->flags == kDocExists);
        CHECK(doc->selectedRev.revID == kExpectedRevID);
        c4doc_release(doc);
    }

    // Update doc:
    {
        auto body = json2fleece("{'ok':'go'}");
        C4DocPutRequest rq = rqTemplate;
        rq.body = body;
        rq.history = &kExpectedRevID;
        rq.historyCount = 1;
        doc = c4doc_put(db, &rq, &commonAncestorIndex, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        CHECK((unsigned long)commonAncestorIndex == 0ul);
        if(isRevTrees()) {
            kExpectedRev2ID = C4STR("2-201796aeeaa6ddbb746d6cab141440f23412ac51");
        } else {
            kExpectedRev2ID = C4STR("2@*");
        }

        CHECK(doc->revID == kExpectedRev2ID);
        CHECK(doc->flags == kDocExists);
        CHECK(doc->selectedRev.revID == kExpectedRev2ID);
        c4doc_release(doc);
    }

    // Insert existing rev that conflicts:
    {
        C4DocPutRequest rq = rqTemplate;
        auto body = json2fleece("{'from':'elsewhere'}");
        rq.body = body;
        rq.existingRevision = true;
        rq.remoteDBID = 1;
        if(isRevTrees()) {
            kConflictRevID = C4STR("2-deadbeef");
        } else {
            kConflictRevID = C4STR("1@cafebabe");
        }

        C4Slice history[2] = {kConflictRevID, kExpectedRevID};
        rq.history = history;
        rq.historyCount = 2;
        rq.allowConflict = true;
        doc = c4doc_put(db, &rq, &commonAncestorIndex, ERROR_INFO(error));

        REQUIRE(doc != nullptr);
        CHECK((unsigned long)commonAncestorIndex == 1ul);
        CHECK(doc->selectedRev.revID == kConflictRevID);
        CHECK(doc->flags == (kDocExists | kDocConflicted));
        // The conflicting rev will now never be the default, even with rev-trees.
        CHECK(doc->revID == kExpectedRev2ID);
        c4doc_release(doc);
    }

    // Delete the document:
    {
        C4DocPutRequest rq = rqTemplate;
        rq.body = nullslice;
        rq.revFlags = kRevDeleted;
        rq.history = &kExpectedRev2ID;
        rq.historyCount = 1;
        doc = c4doc_put(db, &rq, &commonAncestorIndex, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        CHECK(doc->flags == (kDocExists | kDocDeleted | kDocConflicted));
        revID = doc->revID;
        c4doc_release(doc);
    }

    // Resurrect it:
    {
        C4DocPutRequest rq = rqTemplate;
        auto body = json2fleece("{'ok':'again'}");
        rq.body = body;
        rq.history = (C4Slice*)&revID;
        rq.historyCount = 1;
        doc = c4doc_put(db, &rq, &commonAncestorIndex, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        CHECK(doc->flags == (kDocExists | kDocConflicted));
        c4doc_release(doc);
    }
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document create from existing rev", "[Document][C]") {
    C4Error error;
    TransactionHelper t(db);

    // Creating doc given ID:
    C4DocPutRequest rq = {};
    rq.docID = kDocID;
    rq.body = kFleeceBody;
    rq.existingRevision = true;
    C4String history[1] = {kRevID};
    rq.history = history;
    rq.historyCount = 1;
    rq.save = true;
    size_t commonAncestor = 9999;
    auto doc = c4doc_put(db, &rq, &commonAncestor, ERROR_INFO(error));
    REQUIRE(doc != nullptr);
    CHECK(commonAncestor == 1);
    CHECK(doc->docID == kDocID);
    CHECK(doc->revID == kRevID);
    c4doc_release(doc);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Update", "[Document][C]") {
    C4Log("Begin test");
    C4Error error;
    C4Document *doc;

    {
        C4Log("Begin create");
        TransactionHelper t(db);
        doc = c4doc_create(db, kDocID, kFleeceBody, 0, ERROR_INFO(error));
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
    auto doc2 = c4db_getDoc(db, kDocID, false, kDocGetAll, ERROR_INFO(error));
    REQUIRE(doc2->revID == kExpectedRevID);

    // Update it a few times:
    for (int update = 2; update <= 5; ++update) {
        C4Log("Begin save #%d", update);
        TransactionHelper t(db);
        fleece::alloc_slice oldRevID(doc->revID);
        auto updatedDoc = c4doc_update(doc, json2fleece("{'ok':'go'}"), 0, ERROR_INFO(error));
        REQUIRE(updatedDoc);
//        CHECK(doc->selectedRev.revID == oldRevID);
//        CHECK(doc->revID == oldRevID);
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
        CHECK(error == C4Error{LiteCoreDomain, kC4ErrorConflict});
    }

    // Try to create a new doc with the same ID, which will fail:
    {
        C4Log("Begin conflicting create");
        TransactionHelper t(db);
        REQUIRE(c4doc_create(db, kDocID, json2fleece("{'ok':'no way'}"), 0, &error) == nullptr);
        CHECK(error == C4Error{LiteCoreDomain, kC4ErrorConflict});
    }

    c4doc_release(doc);
    c4doc_release(doc2);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Delete then Update", "[Document][C]") {
    TransactionHelper t(db);
    
    // Create a doc:
    C4Error error;
    auto doc = c4doc_create(db, kDocID, kFleeceBody, 0, ERROR_INFO(error));
    REQUIRE(doc);

    // Update the doc:
    auto updatedDoc = c4doc_update(doc, json2fleece("{'ok':'go'}"), 0, ERROR_INFO(error));
    REQUIRE(updatedDoc);
    REQUIRE(updatedDoc->flags == (C4DocumentFlags)(kDocExists));
    c4doc_release(doc);
    doc = updatedDoc;
    
    // Delete the doc:
    updatedDoc = c4doc_update(doc, kC4SliceNull, kRevDeleted, ERROR_INFO(error));
    REQUIRE(updatedDoc);
    REQUIRE(updatedDoc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    c4doc_release(doc);
    doc = updatedDoc;
    
    // Update the doc again:
    updatedDoc = c4doc_update(doc, json2fleece("{'ok':'go'}"), 0, ERROR_INFO(error));
    REQUIRE(updatedDoc);
    REQUIRE(updatedDoc->flags == (C4DocumentFlags)(kDocExists));
    c4doc_release(doc);
    doc = updatedDoc;
    
    c4doc_release(doc);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Conflict", "[Document][C]") {
    C4Error err;
    slice kRev1ID, kRev2ID, kRev3ID, kRev3ConflictID, kRev4ConflictID;
    if (isRevTrees()) {
        kRev1ID = "1-aaaaaa";
        kRev2ID = "2-aaaaaa";
        kRev3ID = "3-aaaaaa";
        kRev3ConflictID = "3-ababab";
        kRev4ConflictID = "4-dddd";
    } else {
        kRev1ID = "1@*";
        kRev2ID = "2@*";
        kRev3ID = "3@*";
        kRev3ConflictID = "3@cafe";
        kRev4ConflictID = "4@cafe";
    }

    const auto kFleeceBody2 = json2fleece("{'ok':'go'}");
    const auto kFleeceBody3 = json2fleece("{'ubu':'roi'}");
    const auto kFleeceBody4 = json2fleece("{'four':'four'}");

    createRev(kDocID, kRev1ID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody2, kRevKeepBody);
    createRev(kDocID, kRev3ID, kFleeceBody3);

    TransactionHelper t(db);

    {
        // "Pull" a conflicting revision:
        C4Slice history[3] = {kRev4ConflictID, kRev3ConflictID, kRev2ID};
        C4DocPutRequest rq = {};
        rq.existingRevision = true;
        rq.docID = kDocID;
        rq.history = history;
        rq.historyCount = 3;
        rq.allowConflict = true;
        rq.body = kFleeceBody4;
        rq.revFlags = kRevKeepBody;
        rq.save = true;
        rq.remoteDBID = 1;

        c4::ref<C4Document> doc = c4doc_put(db, &rq, nullptr, ERROR_INFO(err));
        REQUIRE(doc);
        CHECK(doc->selectedRev.revID == kRev4ConflictID);

        // Check that the local revision is still current:
        CHECK(doc->revID == kRev3ID);
        REQUIRE(c4doc_selectCurrentRevision(doc));
        CHECK(doc->selectedRev.revID == kRev3ID);
        CHECK((int)doc->selectedRev.flags == kRevLeaf);

        if (isRevTrees()) {
            // kRevID -- [kRev2ID] -- kRev3ID
            //                      \ kRev3ConflictID -- [kRev4ConflictID]    [] = remote rev, keep body

            // Check that the pulled revision is treated as a conflict:
            REQUIRE(c4doc_selectRevision(doc, kRev4ConflictID, true, nullptr));
            CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevIsConflict | kRevKeepBody));
            REQUIRE(c4doc_selectParentRevision(doc));
            CHECK((int)doc->selectedRev.flags == kRevIsConflict);

            // Now check the common ancestor algorithm:
            REQUIRE(c4doc_selectCommonAncestorRevision(doc, kRev3ID, kRev4ConflictID));
            CHECK(doc->selectedRev.revID == kRev2ID);

            REQUIRE(c4doc_selectCommonAncestorRevision(doc, kRev4ConflictID, kRev3ID));
            CHECK(doc->selectedRev.revID == kRev2ID);

            REQUIRE(c4doc_selectCommonAncestorRevision(doc, kRev3ConflictID, kRev3ID));
            CHECK(doc->selectedRev.revID == kRev2ID);
            REQUIRE(c4doc_selectCommonAncestorRevision(doc, kRev3ID, kRev3ConflictID));
            CHECK(doc->selectedRev.revID == kRev2ID);

            REQUIRE(c4doc_selectCommonAncestorRevision(doc, kRev2ID, kRev3ID));
            CHECK(doc->selectedRev.revID == kRev2ID);
            REQUIRE(c4doc_selectCommonAncestorRevision(doc, kRev3ID, kRev2ID));
            CHECK(doc->selectedRev.revID == kRev2ID);

            REQUIRE(c4doc_selectCommonAncestorRevision(doc, kRev2ID, kRev2ID));
            CHECK(doc->selectedRev.revID == kRev2ID);
        } else {
            // Check that the pulled revision is treated as a conflict:
            REQUIRE(c4doc_selectRevision(doc, kRev4ConflictID, true, nullptr));
            CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevIsConflict));
        }
    }

    auto mergedBody = json2fleece("{\"merged\":true}");

    {
        C4Log("--- Resolve, remote wins");
        c4::ref<C4Document> doc = c4doc_get(db, kDocID, true, ERROR_INFO(err));
        REQUIRE(c4doc_resolveConflict(doc, kRev4ConflictID, kRev3ID, nullslice, 0, WITH_ERROR(&err)));
        c4doc_selectCurrentRevision(doc);
        CHECK(docBodyEquals(doc, kFleeceBody4));
        if (isRevTrees()) {
            // kRevID -- kRev2ID -- kRev3ConflictID -- kMergedRevID
            CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevKeepBody));
            CHECK(doc->selectedRev.revID == kRev4ConflictID);
            alloc_slice revHistory(c4doc_getRevisionHistory(doc, 99, nullptr, 0));
            CHECK(revHistory == "4-dddd,3-ababab,2-aaaaaa,1-aaaaaa"_sl);

            c4doc_selectParentRevision(doc);
            CHECK(doc->selectedRev.revID == kRev3ConflictID);
            CHECK((int)doc->selectedRev.flags == 0);

            c4doc_selectParentRevision(doc);
            CHECK(doc->selectedRev.revID == kRev2ID);
            CHECK((int)doc->selectedRev.flags == 0);
        } else {
            CHECK((int)doc->selectedRev.flags == kRevLeaf);
            CHECK(doc->selectedRev.revID == "4@cafe"_sl);
            alloc_slice vector(c4doc_getRevisionHistory(doc, 0, nullptr, 0));
            CHECK(vector == "4@cafe,2@*"_sl);
        }

        CHECK(c4doc_selectRevision(doc, kRev4ConflictID, false, nullptr));
        CHECK(! c4doc_selectRevision(doc, kRev3ID, false, nullptr));
    }

    if (!isRevTrees()) {
        C4Log("--- Resolve, remote wins but merge vectors");
        // We have to update the local revision to get into this state.
        // Note we are NOT saving the doc, so we don't mess up the following test block.
        slice kSomeoneElsesVersion = "7@1a1a";
        C4Slice history[] = {kSomeoneElsesVersion, kRev3ID};
        C4DocPutRequest rq = {};
        rq.existingRevision = true;
        rq.docID = kDocID;
        rq.history = history;
        rq.historyCount = 2;
        rq.body = kFleeceBody2;
        c4::ref<C4Document> doc = c4doc_put(db, &rq, nullptr, ERROR_INFO(err));
        REQUIRE(doc);

        REQUIRE(c4doc_resolveConflict(doc, kRev4ConflictID, kSomeoneElsesVersion, nullslice, 0, WITH_ERROR(&err)));
        c4doc_selectCurrentRevision(doc);
        CHECK(docBodyEquals(doc, kFleeceBody4));

        CHECK((int)doc->selectedRev.flags == kRevLeaf);
        CHECK(doc->selectedRev.revID == "4@*"_sl);
        alloc_slice vector(c4doc_getRevisionHistory(doc, 0, nullptr, 0));
        CHECK(vector == "4@*,4@cafe,7@1a1a"_sl);
        CHECK(c4doc_selectRevision(doc, kRev4ConflictID, false, nullptr));
        CHECK(! c4doc_selectRevision(doc, kRev3ID, false, nullptr));
    }

    {
        C4Log("--- Merge onto remote");
        c4::ref<C4Document> doc = c4doc_get(db, kDocID, true, ERROR_INFO(err));
        REQUIRE(c4doc_resolveConflict(doc, kRev4ConflictID, kRev3ID, mergedBody, 0, WITH_ERROR(&err)));
        c4doc_selectCurrentRevision(doc);
        CHECK(docBodyEquals(doc, mergedBody));
        if (isRevTrees()) {
            // kRevID -- kRev2ID -- kRev3ConflictID -- [kRev4ConflictID] -- kMergedRevID
            CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevNew));
            CHECK(doc->selectedRev.revID == "5-40c76a18ad61e00aa6372396a8d03a023c401fe3"_sl);
            alloc_slice revHistory(c4doc_getRevisionHistory(doc, 99, nullptr, 0));
            CHECK(revHistory == "5-40c76a18ad61e00aa6372396a8d03a023c401fe3,4-dddd,3-ababab,2-aaaaaa,1-aaaaaa"_sl);

            c4doc_selectParentRevision(doc);
            CHECK(doc->selectedRev.revID == kRev4ConflictID);
            CHECK((int)doc->selectedRev.flags == kRevKeepBody);

            c4doc_selectParentRevision(doc);
            CHECK(doc->selectedRev.revID == kRev3ConflictID);
            CHECK((int)doc->selectedRev.flags == 0);

            c4doc_selectParentRevision(doc);
            CHECK(doc->selectedRev.revID == kRev2ID);
            CHECK((int)doc->selectedRev.flags == 0);
        } else {
            CHECK((int)doc->selectedRev.flags == kRevLeaf);
            CHECK(doc->selectedRev.revID == "4@*"_sl);
            alloc_slice vector(c4doc_getRevisionHistory(doc, 0, nullptr, 0));
            CHECK(vector == "4@*,4@cafe"_sl);
        }

        CHECK(! c4doc_selectRevision(doc, kRev3ID, false, nullptr));
    }

    {
        C4Log("--- Resolve, local wins");
        c4::ref<C4Document> doc = c4doc_get(db, kDocID, true, ERROR_INFO(err));
        REQUIRE(doc);
        REQUIRE(c4doc_resolveConflict(doc, kRev3ID, kRev4ConflictID, nullslice, 0, WITH_ERROR(&err)));
        // kRevID -- [kRev2ID] -- kRev3ID
        c4doc_selectCurrentRevision(doc);
        CHECK(docBodyEquals(doc, kFleeceBody3));
        if (isRevTrees()) {
            CHECK((int)doc->selectedRev.flags == kRevLeaf);
            CHECK(doc->selectedRev.revID == kRev3ID);

            c4doc_selectParentRevision(doc);
            CHECK(doc->selectedRev.revID == kRev2ID);
            CHECK((int)doc->selectedRev.flags == kRevKeepBody);

            CHECK(! c4doc_selectRevision(doc, kRev4ConflictID, false, nullptr));
            CHECK(! c4doc_selectRevision(doc, kRev3ConflictID, false, nullptr));
        } else {
            CHECK((int)doc->selectedRev.flags == kRevLeaf);
            CHECK(doc->selectedRev.revID == "4@*"_sl);
            alloc_slice vector(c4doc_getRevisionHistory(doc, 0, nullptr, 0));
            CHECK(vector == "4@*,4@cafe"_sl);
        }
    }

    {
        C4Log("--- Merge onto local");
        c4::ref<C4Document> doc = c4doc_get(db, kDocID, true, ERROR_INFO(err));
        REQUIRE(doc);
        REQUIRE(c4doc_resolveConflict(doc, kRev3ID, kRev4ConflictID, mergedBody, 0, WITH_ERROR(&err)));
        // kRevID -- [kRev2ID] -- kRev3ID -- kMergedRevID
        c4doc_selectCurrentRevision(doc);
        CHECK(docBodyEquals(doc, mergedBody));
        if (isRevTrees()) {
            CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevNew));
            CHECK(doc->selectedRev.revID == "4-41cdb6e71647962c281333ceac7b36c46b65b41f"_sl);

            c4doc_selectParentRevision(doc);
            CHECK(doc->selectedRev.revID == kRev3ID);
            CHECK((int)doc->selectedRev.flags == 0);

            c4doc_selectParentRevision(doc);
            CHECK(doc->selectedRev.revID == kRev2ID);
            CHECK((int)doc->selectedRev.flags == kRevKeepBody);

            CHECK(! c4doc_selectRevision(doc, kRev4ConflictID, false, nullptr));
            CHECK(! c4doc_selectRevision(doc, kRev3ConflictID, false, nullptr));
        } else {
            CHECK((int)doc->selectedRev.flags == kRevLeaf);
            CHECK(doc->selectedRev.revID == "4@*"_sl);
            alloc_slice vector(c4doc_getRevisionHistory(doc, 0, nullptr, 0));
            CHECK(vector == "4@*,4@cafe"_sl);
        }
    }

}


N_WAY_TEST_CASE_METHOD(C4Test, "Document from Fleece", "[Document][C]") {
    if (!isRevTrees())
        return;

    CHECK(c4doc_containingValue((FLValue)0x12345678) == nullptr);

    const auto kFleeceBody = json2fleece("{'ubu':'roi'}");
    createRev(kDocID, kRevID, kFleeceBody);

    C4Document* doc = c4doc_get(db, kDocID, true, nullptr);
    REQUIRE(doc);
    FLValue root = FLValue(c4doc_getProperties(doc));
    REQUIRE(root);
    CHECK(c4doc_containingValue(root) == doc);
    FLValue ubu = FLDict_Get(FLValue_AsDict(root), "ubu"_sl);
    CHECK(c4doc_containingValue(ubu) == doc);
    c4doc_release(doc);

    CHECK(c4doc_containingValue(root) == nullptr);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Leaf Document from Fleece", "[Document][C]") {
    if (!isRevTrees())
        return;

    CHECK(c4doc_containingValue((FLValue)0x12345678) == nullptr);

    const auto kFleeceBody = json2fleece("{'ubu':'roi'}");
    createRev(kDocID, kRevID, kFleeceBody);

    C4Document* doc = c4db_getDoc(db, kDocID, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc);
    CHECK(doc->selectedRev.revID == kRevID);
    FLValue root = FLValue(c4doc_getProperties(doc));
    REQUIRE(root);
    CHECK(c4doc_containingValue(root) == doc);
    FLValue ubu = FLDict_Get(FLValue_AsDict(root), "ubu"_sl);
    CHECK(c4doc_containingValue(ubu) == doc);
    c4doc_release(doc);

    CHECK(c4doc_containingValue(root) == nullptr);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties", "[Document][C][Blob]") {
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
    REQUIRE((FLValue_AsString(testVal) == blobSl));

    REQUIRE(FLValue_FindDoc((FLValue)d) == result);
    CHECK(c4doc_dictContainsBlobs(d));
    FLDoc_Release(result);
    
    enc = c4db_getSharedFleeceEncoder(db);
    FLEncoder_BeginDict(enc, 0);
    FLEncoder_EndDict(enc);
    result = FLEncoder_FinishDoc(enc, nullptr);
    REQUIRE(result);
    val = FLDoc_GetRoot(result);
    d = FLValue_AsDict(val);
    REQUIRE(d);
    
    CHECK(!c4doc_dictContainsBlobs(d));
    FLDoc_Release(result);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties 2", "[Document][C]") {
    // Check that old meta properties get removed:
    TransactionHelper t(db);
    auto sk = c4db_getFLSharedKeys(db);
    auto dict = json2dict("{_id:'foo', _rev:'1-2345', x:17}");
    CHECK(c4doc_hasOldMetaProperties(dict));
    alloc_slice stripped = c4doc_encodeStrippingOldMetaProperties(dict, sk, NULL);
    Doc doc(stripped, kFLTrusted, sk);
    CHECK(fleece2json(stripped) == "{x:17}");
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties 3", "[Document][Blob][C]") {
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


N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties 4", "[Document][Blob][C]") {
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


N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties 5", "[Document][Blob][C]") {
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
N_WAY_TEST_CASE_METHOD(C4Test, "Document Clobber Remote Rev", "[Document][C]") {

    if (!isRevTrees())
        return;

    TransactionHelper t(db);

    // Write doc to db
    createRev(kDocID, kRevID, kFleeceBody);

    // Use default remote id
    C4RemoteID testRemoteId = 1;

    // Read doc from db and keep in memory
    C4Error error;
    auto curDoc = c4db_getDoc(db, kDocID, false, kDocGetAll, ERROR_INFO(error));
    REQUIRE(curDoc != nullptr);

    // Call MarkRevSynced which will set the flag
    bool markSynced = c4db_markSynced(db, kDocID, curDoc->revID, curDoc->sequence,
                                      testRemoteId, ERROR_INFO(error));
    REQUIRE(markSynced);

    // Get the latest version of the doc
    auto curDocAfterMarkSync = c4db_getDoc(db, kDocID, true, kDocGetAll, ERROR_INFO(error));
    REQUIRE(curDocAfterMarkSync != nullptr);

    // Get the remote ancesor rev, and make sure it matches up with the latest rev of the doc
    alloc_slice remoteRevID = c4doc_getRemoteAncestor(curDocAfterMarkSync, testRemoteId);
    REQUIRE(remoteRevID == curDocAfterMarkSync->revID);

    // Update doc -- before the bugfix, this was clobbering the remote ancestor rev
    auto updatedDoc = c4doc_update(curDoc, json2fleece("{'ok':'go'}"), 0, ERROR_INFO(error));
    REQUIRE(updatedDoc);

    // Re-read the doc from the db just to be sure getting accurate version
    auto updatedDocRefreshed = c4db_getDoc(db, kDocID, true, kDocGetAll, ERROR_INFO(error));

    // Check the remote ancestor rev of the updated doc and make sure it has not been clobbered.
    // Before the bug fix for LiteCore #478, this was returning an empty value.
    alloc_slice remoteRevIDAfterupdate = c4doc_getRemoteAncestor(updatedDocRefreshed, testRemoteId);
    REQUIRE(remoteRevIDAfterupdate == curDocAfterMarkSync->revID);

    // Cleanup
    c4doc_release(curDoc);
    c4doc_release(curDocAfterMarkSync);
    c4doc_release(updatedDoc);
    c4doc_release(updatedDocRefreshed);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Global Rev ID", "[Document][C]") {
    createRev(kDocID, kRevID, kFleeceBody);
    C4Document *doc = c4doc_get(db, kDocID, true, nullptr);
    alloc_slice revID = c4doc_getSelectedRevIDGlobalForm(doc);
    C4Log("Global rev ID = %.*s", (int)revID.size, (char*)revID.buf);
    CHECK(revID.findByte('*') == nullptr);
    c4doc_release(doc);
}
