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
#include "c4Private.h"
#include "Benchmark.hh"
#include "c4Document+Fleece.h"

N_WAY_TEST_CASE_METHOD(C4Test, "Invalid docID", "[Database][C]") {
    c4log_warnOnErrors(false);
    TransactionHelper t(db);

    auto checkPutBadDocID = [this](C4Slice docID) {
        C4Error error;
        C4DocPutRequest rq = {};
        rq.body = C4Test::kBody;
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

    createRev(kDocID, kRevID, kBody);
    createRev(kDocID, kRev2ID, kBody);
    createRev(kDocID, kRev3ID, kBody);

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
    c4doc_free(doc);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document CreateVersionedDoc", "[Database][C]") {
    // Try reading doc with mustExist=true, which should fail:
    C4Error error;
    C4Document* doc;
    doc = c4doc_get(db, kDocID, true, &error);
    REQUIRE(!doc);
    REQUIRE((uint32_t)error.domain == (uint32_t)LiteCoreDomain);
    REQUIRE(error.code == (int)kC4ErrorNotFound);
    c4doc_free(doc);

    // Now get the doc with mustExist=false, which returns an empty doc:
    doc = c4doc_get(db, kDocID, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->flags == 0);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID.buf == 0);
    REQUIRE(doc->selectedRev.revID.buf == 0);
    c4doc_free(doc);

    {
        TransactionHelper t(db);
        C4DocPutRequest rq = {};
        rq.revFlags = kRevKeepBody;
        rq.existingRevision = true;
        rq.docID = kDocID;
        rq.history = &kRevID;
        rq.historyCount = 1;
        rq.body = kBody;
        rq.save = true;
        doc = c4doc_put(db, &rq, nullptr, &error);
        REQUIRE(doc != nullptr);
        REQUIRE(doc->revID == kRevID);
        REQUIRE(doc->selectedRev.revID == kRevID);
        REQUIRE(doc->selectedRev.flags == (kRevKeepBody | kRevLeaf));
        REQUIRE(doc->selectedRev.body == kBody);
        c4doc_free(doc);
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
    REQUIRE(doc->selectedRev.body == kBody);
    c4doc_free(doc);

    // Get the doc by its sequence:
    doc = c4doc_getBySequence(db, 1, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->sequence == 1);
    REQUIRE(doc->flags == kDocExists);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRevID);
    REQUIRE(doc->selectedRev.revID == kRevID);
    REQUIRE(doc->selectedRev.sequence == 1);
    REQUIRE(doc->selectedRev.body == kBody);
    {
        TransactionHelper t(db);
        REQUIRE(c4doc_removeRevisionBody(doc));
        REQUIRE(c4doc_selectCurrentRevision(doc));
    }
    
    REQUIRE(doc->selectedRev.body.buf == nullptr);
    REQUIRE(doc->selectedRev.body.size == 0);
    c4doc_free(doc);

    // Get a bogus sequence
    doc = c4doc_getBySequence(db, 2, &error);
    CHECK(doc == nullptr);
    CHECK(error.domain == LiteCoreDomain);
    CHECK(error.code == kC4ErrorNotFound);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document CreateMultipleRevisions", "[Database][C]") {
    const C4Slice kBody2 = C4STR("{\"ok\":\"go\"}");
    const C4Slice kBody3 = C4STR("{\"ubu\":\"roi\"}");
    createRev(kDocID, kRevID, kBody);
    createRev(kDocID, kRev2ID, kBody2, kRevKeepBody);
    createRev(kDocID, kRev2ID, kBody2); // test redundant insert

    // Reload the doc:
    C4Error error;
    C4Document *doc = c4doc_get(db, kDocID, true, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->flags == kDocExists);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRev2ID);
    REQUIRE(doc->selectedRev.revID == kRev2ID);
    REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)2);
    REQUIRE(doc->selectedRev.body == kBody2);

    if (versioning() == kC4RevisionTrees) {
        // Select 1st revision:
        REQUIRE(c4doc_selectParentRevision(doc));
        REQUIRE(doc->selectedRev.revID == kRevID);
        REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)1);
        REQUIRE(doc->selectedRev.body == kC4SliceNull);
        REQUIRE(!c4doc_hasRevisionBody(doc));
        REQUIRE(!c4doc_selectParentRevision(doc));
        c4doc_free(doc);

        // Add a 3rd revision:
        createRev(kDocID, kRev3ID, kBody3);
        // Revision 2 should keep its body due to the kRevKeepBody flag:
        doc = c4doc_get(db, kDocID, true, &error);
        REQUIRE(doc != nullptr);
        REQUIRE(c4doc_selectParentRevision(doc));
        REQUIRE(doc->selectedRev.revID == kRev2ID);
        REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)2);
        REQUIRE(doc->selectedRev.flags == kRevKeepBody);
        REQUIRE(doc->selectedRev.body == kBody2);
        c4doc_free(doc);

        // Purge doc
        {
            TransactionHelper t(db);
            doc = c4doc_get(db, kDocID, true, &error);
            int nPurged = c4doc_purgeRevision(doc, {}, &error);
            REQUIRE(nPurged == 3);
            REQUIRE(c4doc_save(doc, 20, &error));
            c4doc_free(doc);
        }

        // Make sure it's gone:
        doc = c4doc_get(db, kDocID, true, &error);
        CHECK(!doc);
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorNotFound);
    }
    c4doc_free(doc);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Purge", "[Database][C]") {
    const C4Slice kBody2 = C4STR("{\"ok\":\"go\"}");
    const C4Slice kBody3 = C4STR("{\"ubu\":\"roi\"}");
    createRev(kDocID, kRevID, kBody);
    createRev(kDocID, kRev2ID, kBody2);
    createRev(kDocID, kRev3ID, kBody3);
    
    C4Slice history[3] = {C4STR("3-ababab"), kRev2ID};
    C4DocPutRequest rq = {};
    rq.existingRevision = true;
    rq.docID = kDocID;
    rq.history = history;
    rq.historyCount = 2;
    rq.body = kBody3;
    rq.save = true;
    C4Error err;
    REQUIRE(c4db_beginTransaction(db, &err));
    auto doc = c4doc_put(db, &rq, nullptr, &err);
    REQUIRE(doc);
    c4doc_free(doc);
    REQUIRE(c4db_endTransaction(db, true, &err));
    
    REQUIRE(c4db_beginTransaction(db, &err));
    REQUIRE(c4db_purgeDoc(db, kDocID, &err));
    REQUIRE(c4db_endTransaction(db, true, &err));
    
    REQUIRE(c4db_getDocumentCount(db) == 0);
    
    createRev(kDocID, kRevID, kBody);
    createRev(kDocID, kRev2ID, kBody2);
    createRev(kDocID, kRev3ID, kBody3);
    REQUIRE(c4db_beginTransaction(db, &err));
    doc = c4doc_put(db, &rq, nullptr, &err);
    REQUIRE(doc);
    REQUIRE(c4db_endTransaction(db, true, &err));
    
    REQUIRE(c4db_beginTransaction(db, &err));
    CHECK(c4doc_purgeRevision(doc, kRev2ID, &err) == 0);
    REQUIRE(c4doc_purgeRevision(doc, kC4SliceNull, &err) == 4);
    REQUIRE(c4doc_save(doc, 20, &err));
    c4doc_free(doc);
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

    static const unsigned kNumRevs = 10000;
    fleece::Stopwatch st;
    C4Error error;
    auto doc = c4doc_get(db, kDocID, false, &error);
    {
        TransactionHelper t(db);
        REQUIRE(doc != nullptr);
        for (unsigned i = 0; i < kNumRevs; i++) {
            C4DocPutRequest rq = {};
            rq.docID = doc->docID;
            rq.history = &doc->revID;
            rq.historyCount = 1;
            rq.body = kBody;
            if (i == 0) {
                // Pretend the 1st revision has a remote origin (see issue #376)
                rq.remoteDBID = 1;
                rq.existingRevision = true;
                rq.history = &kRevID;
            }
            rq.save = true;
            auto savedDoc = c4doc_put(db, &rq, nullptr, &error);
            REQUIRE(savedDoc != nullptr);
            c4doc_free(doc);
            doc = savedDoc;
        }
    }
    C4Log("Created %u revisions in %.3f sec", kNumRevs, st.elapsed());

    // Check rev tree depth:
    unsigned nRevs = 0;
    c4doc_selectCurrentRevision(doc);
    do {
        if (isRevTrees())
            CHECK(c4rev_getGeneration(doc->selectedRev.revID) == kNumRevs - nRevs);
        ++nRevs;
    } while (c4doc_selectParentRevision(doc));
    C4Log("Document rev tree depth is %u", nRevs);
    if (isRevTrees())
        REQUIRE(nRevs == 30);

    c4doc_free(doc);
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
    c4doc_free(doc);

    // Creating doc, no ID:
    doc = c4doc_getForPut(db, kC4SliceNull, kC4SliceNull, false, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID.size >= 20);  // Verify it got a random doc ID
    REQUIRE(doc->revID == kC4SliceNull);
    REQUIRE(doc->flags == 0);
    REQUIRE(doc->selectedRev.revID == kC4SliceNull);
    c4doc_free(doc);

    // Delete with no revID given
    doc = c4doc_getForPut(db, kDocID, kC4SliceNull, true/*deleting*/, false, &error);
    REQUIRE(doc == nullptr);
    REQUIRE(error.code == kC4ErrorNotFound);

    // Adding new rev of nonexistent doc:
    doc = c4doc_getForPut(db, kDocID, kRevID, false, false, &error);
    REQUIRE(doc == nullptr);
    REQUIRE(error.code == kC4ErrorNotFound);

    // Adding new rev of existing doc:
    createRev(kDocID, kRevID, kBody);
    doc = c4doc_getForPut(db, kDocID, kRevID, false, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRevID);
    REQUIRE(doc->flags == kDocExists);
    REQUIRE(doc->selectedRev.revID == kRevID);
    c4doc_free(doc);

    // Adding new rev, with nonexistent parent:
    doc = c4doc_getForPut(db, kDocID, kRev2ID, false, false, &error);
    REQUIRE(doc == nullptr);
    REQUIRE(error.code == kC4ErrorConflict);

    // Conflict -- try & fail to update non-current rev:
    const C4Slice kBody2 = C4STR("{\"ok\":\"go\"}");
    createRev(kDocID, kRev2ID, kBody2);
    doc = c4doc_getForPut(db, kDocID, kRevID, false, false, &error);
    REQUIRE(doc == nullptr);
    REQUIRE(error.code == kC4ErrorConflict);

    if (isRevTrees()) {
        // Conflict -- force an update of non-current rev:
        doc = c4doc_getForPut(db, kDocID, kRevID, false, true/*allowConflicts*/, &error);
        REQUIRE(doc != nullptr);
        REQUIRE(doc->docID == kDocID);
        REQUIRE(doc->selectedRev.revID == kRevID);
        c4doc_free(doc);
    }

    // Deleting the doc:
    doc = c4doc_getForPut(db, kDocID, kRev2ID, true/*deleted*/, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->selectedRev.revID == kRev2ID);
    c4doc_free(doc);
    
    // Actually delete it:
    createRev(kDocID, kRev3ID, kC4SliceNull, kRevDeleted);

    // Re-creating the doc (no revID given):
    doc = c4doc_getForPut(db, kDocID, kC4SliceNull, false, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRev3ID);
    REQUIRE(doc->flags == (kDocExists | kDocDeleted));
    REQUIRE(doc->selectedRev.revID == kRev3ID);
    c4doc_free(doc);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Put", "[Database][C]") {
    C4Error error;
    TransactionHelper t(db);

    // Creating doc given ID:
    C4DocPutRequest rq = {};
    rq.docID = kDocID;
    rq.body = kBody;
    rq.save = true;
    auto doc = c4doc_put(db, &rq, nullptr, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    C4Slice kExpectedRevID = isRevTrees() ? C4STR("1-9a57afaa2e551a0bc470548763a5660a19d579f4")
                                          : C4STR("1@*");
    REQUIRE(doc->revID == kExpectedRevID);
    REQUIRE(doc->flags == kDocExists);
    REQUIRE(doc->selectedRev.revID == kExpectedRevID);
    c4doc_free(doc);

    // Update doc:
    rq.body = C4STR("{\"ok\":\"go\"}");
    rq.history = &kExpectedRevID;
    rq.historyCount = 1;
    size_t commonAncestorIndex;
    doc = c4doc_put(db, &rq, &commonAncestorIndex, &error);
    REQUIRE(doc != nullptr);
    REQUIRE((unsigned long)commonAncestorIndex == 0ul);
    C4Slice kExpectedRev2ID = isRevTrees() ? C4STR("2-559298a253c7bfb7edc0ce1dc93c8e8ebf504065")
                                           : C4STR("2@*");
    REQUIRE(doc->revID == kExpectedRev2ID);
    REQUIRE(doc->flags == kDocExists);
    REQUIRE(doc->selectedRev.revID == kExpectedRev2ID);
    c4doc_free(doc);

    // Insert existing rev that conflicts:
    rq.body = C4STR("{\"from\":\"elsewhere\"}");
    rq.existingRevision = true;
    C4Slice kConflictRevID = isRevTrees() ? C4STR("2-deadbeef")
                                          : C4STR("1@binky");
    C4Slice history[2] = {kConflictRevID, kExpectedRevID};
    rq.history = history;
    rq.historyCount = 2;
    doc = c4doc_put(db, &rq, &commonAncestorIndex, &error);
    REQUIRE(doc != nullptr);
    REQUIRE((unsigned long)commonAncestorIndex == 1ul);
    REQUIRE(doc->selectedRev.revID == kConflictRevID);
    REQUIRE(doc->flags == (kDocExists | kDocConflicted));
    // The conflicting rev will now never be the default, even with rev-trees.
    REQUIRE(doc->revID == kExpectedRev2ID);
    
    C4SliceResult body = c4doc_detachRevisionBody(doc);
    CHECK(body == rq.body);
    CHECK(!doc->selectedRev.body.buf);
    c4doc_free(doc);
    c4slice_free(body);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Update", "[Database][C]") {
    C4Log("Begin test");
    C4Error error;
    C4Document *doc;

    {
        C4Log("Begin create");
        TransactionHelper t(db);
        doc = c4doc_create(db, kDocID, kBody, 0, &error);
        REQUIRE(doc);
    }
    C4Log("After save");
    C4Slice kExpectedRevID = isRevTrees() ? C4STR("1-9a57afaa2e551a0bc470548763a5660a19d579f4")
                                          : C4STR("1@*");
    REQUIRE(doc->revID == kExpectedRevID);
    REQUIRE(doc->flags == kDocExists);
    REQUIRE(doc->selectedRev.revID == kExpectedRevID);
    REQUIRE(doc->docID == kDocID);

    // Read the doc into another C4Document:
    auto doc2 = c4doc_get(db, kDocID, false, &error);
    REQUIRE(doc2->revID == kExpectedRevID);

    // Update it a few times:
    for (int update = 2; update <= 5; ++update) {
        C4Log("Begin save #%d", update);
        TransactionHelper t(db);
        fleece::alloc_slice oldRevID = doc->revID;
        auto updatedDoc = c4doc_update(doc, C4STR("{\"ok\":\"go\"}"), 0, &error);
        REQUIRE(updatedDoc);
        REQUIRE(doc->selectedRev.revID == oldRevID);
        REQUIRE(doc->revID == oldRevID);
        c4doc_free(doc);
        doc = updatedDoc;
    }
    C4Log("After multiple updates");
    C4Slice kExpectedRev2ID = isRevTrees() ? C4STR("5-b0a49dc580ffd380050fe54d26d380d270ad275f")
                                           : C4STR("5@*");
    REQUIRE(doc->revID == kExpectedRev2ID);
    REQUIRE(doc->selectedRev.revID == kExpectedRev2ID);

    // Now try to update the other C4Document, which will fail:
    {
        C4Log("Begin conflicting save");
        TransactionHelper t(db);
        REQUIRE(c4doc_update(doc2, C4STR("{\"ok\":\"no way\"}"), 0, &error) == nullptr);
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorConflict);
    }

    // Try to create a new doc with the same ID, which will fail:
    {
        C4Log("Begin conflicting create");
        TransactionHelper t(db);
        REQUIRE(c4doc_create(db, kDocID, C4STR("{\"ok\":\"no way\"}"), 0, &error) == nullptr);
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorConflict);
    }

    c4doc_free(doc);
    c4doc_free(doc2);
}


N_WAY_TEST_CASE_METHOD(C4Test, "Document Conflict", "[Database][C]") {
    if (!isRevTrees())
        return;

    const C4Slice kBody2 = C4STR("{\"ok\":\"go\"}");
    const C4Slice kBody3 = C4STR("{\"ubu\":\"roi\"}");
    createRev(kDocID, kRevID, kBody);
    createRev(kDocID, kRev2ID, kBody2, kRevKeepBody);
    createRev(kDocID, C4STR("3-aaaaaa"), kBody3);

    TransactionHelper t(db);

    // "Pull" a conflicting revision:
    C4Slice history[3] = {C4STR("4-dddd"), C4STR("3-ababab"), kRev2ID};
    C4DocPutRequest rq = {};
    rq.existingRevision = true;
    rq.docID = kDocID;
    rq.history = history;
    rq.historyCount = 3;
    rq.body = kBody3;
    rq.save = true;
    C4Error err;
    auto doc = c4doc_put(db, &rq, nullptr, &err);
    REQUIRE(doc);

    // Check that the pulled revision is treated as a conflict:
    CHECK(doc->selectedRev.revID == C4STR("4-dddd"));
    CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevIsConflict));
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK((int)doc->selectedRev.flags == kRevIsConflict);

    // Check that the local revision is still current:
    CHECK(doc->revID == C4STR("3-aaaaaa"));
    REQUIRE(c4doc_selectCurrentRevision(doc));
    CHECK(doc->selectedRev.revID == C4STR("3-aaaaaa"));
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

    SECTION("Merge, 4 wins") {
        REQUIRE(c4doc_resolveConflict(doc, C4STR("4-dddd"), C4STR("3-aaaaaa"),
                                      C4STR("{\"merged\":true}"), &err));
        c4doc_selectCurrentRevision(doc);
        CHECK(doc->selectedRev.revID == C4STR("5-940fe7e020dbf8db0f82a5d764870c4b6c88ae99"));
        CHECK(doc->selectedRev.body == C4STR("{\"merged\":true}"));
        CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevNew));
        c4doc_selectParentRevision(doc);
        CHECK(doc->selectedRev.revID == C4STR("4-dddd"));
        CHECK((int)doc->selectedRev.flags == 0);
        c4doc_selectParentRevision(doc);
        CHECK(doc->selectedRev.revID == C4STR("3-ababab"));
        CHECK((int)doc->selectedRev.flags == 0);
    }

    SECTION("Merge, 3 wins") {
        REQUIRE(c4doc_resolveConflict(doc, C4STR("3-aaaaaa"), C4STR("4-dddd"),
                                      C4STR("{\"merged\":true}"), &err));
        c4doc_selectCurrentRevision(doc);
        CHECK(doc->selectedRev.revID == C4STR("4-333ee0677b5f1e1e5064b050d417a31d2455dc30"));
        CHECK(doc->selectedRev.body == C4STR("{\"merged\":true}"));
        CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevNew));
        c4doc_selectParentRevision(doc);
        CHECK(doc->selectedRev.revID == C4STR("3-aaaaaa"));
        CHECK((int)doc->selectedRev.flags == 0);
    }

    c4doc_free(doc);
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
    
    FLSliceResult result = FLEncoder_Finish(enc, nullptr);
    REQUIRE(result.buf);
    FLValue val = FLValue_FromTrustedData((FLSlice)result);
    FLDict d = FLValue_AsDict(val);
    REQUIRE(d);
    
    FLDictKey testKey = c4db_initFLDictKey(db, C4STR("@type"));
    FLValue testVal = FLDict_GetWithKey(d, &testKey);
    
    FLSlice blobSl = FLSTR("blob"); // Windows cannot compile this inside of a REQUIRE
    REQUIRE(FLValue_AsString(testVal) == blobSl);
    
    CHECK(c4doc_dictContainsBlobs(d, c4db_getFLSharedKeys(db)));
    FLSliceResult_Free(result);
    
    enc = c4db_getSharedFleeceEncoder(db);
    FLEncoder_BeginDict(enc, 0);
    FLEncoder_EndDict(enc);
    result = FLEncoder_Finish(enc, nullptr);
    REQUIRE(result.buf);
    val = FLValue_FromTrustedData((FLSlice)result);
    d = FLValue_AsDict(val);
    REQUIRE(d);
    
    CHECK(!c4doc_dictContainsBlobs(d, nullptr));
    FLSliceResult_Free(result);
}
