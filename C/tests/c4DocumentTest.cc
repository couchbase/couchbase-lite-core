//
//  c4DocumentTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/24/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4Private.h"


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
    REQUIRE(doc->flags == (C4DocumentFlags)0);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID.buf == 0);
    REQUIRE(doc->selectedRev.revID.buf == 0);
    c4doc_free(doc);

    {
        TransactionHelper t(db);
        C4DocPutRequest rq = {};
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
        REQUIRE(doc->selectedRev.flags == (C4RevisionFlags)kRevLeaf);
        REQUIRE(doc->selectedRev.body == kBody);
        c4doc_free(doc);
    }

    // Reload the doc:
    doc = c4doc_get(db, kDocID, true, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->flags == (C4DocumentFlags)kExists);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRevID);
    REQUIRE(doc->selectedRev.revID == kRevID);
    REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)1);
    REQUIRE(doc->selectedRev.body == kBody);
    c4doc_free(doc);

    // Get the doc by its sequence:
    doc = c4doc_getBySequence(db, 1, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->flags == (C4DocumentFlags)kExists);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRevID);
    REQUIRE(doc->selectedRev.revID == kRevID);
    REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)1);
    REQUIRE(doc->selectedRev.body == kBody);
    c4doc_free(doc);
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
    REQUIRE(doc->flags == (C4DocumentFlags)kExists);
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
            int nPurged = c4doc_purgeRevision(doc, kRev3ID, &error);
            REQUIRE(nPurged == 3);
            REQUIRE(c4doc_save(doc, 20, &error));
        }
    }
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
    REQUIRE(doc->flags == (C4DocumentFlags)0);
    REQUIRE(doc->selectedRev.revID == kC4SliceNull);
    c4doc_free(doc);

    // Creating doc, no ID:
    doc = c4doc_getForPut(db, kC4SliceNull, kC4SliceNull, false, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID.size >= 20);  // Verify it got a random doc ID
    REQUIRE(doc->revID == kC4SliceNull);
    REQUIRE(doc->flags == (C4DocumentFlags)0);
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
    REQUIRE(doc->flags == (C4DocumentFlags)kExists);
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
    REQUIRE(doc->flags == (C4DocumentFlags)(kExists | kDeleted));
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
    C4Slice kExpectedRevID = isRevTrees() ? C4STR("1-c10c25442d9fe14fa3ca0db4322d7f1e43140fab")
                                          : C4STR("1@*");
    REQUIRE(doc->revID == kExpectedRevID);
    REQUIRE(doc->flags == (C4DocumentFlags)kExists);
    REQUIRE(doc->selectedRev.revID == kExpectedRevID);
    c4doc_free(doc);

    // Update doc:
    rq.body = C4STR("{\"ok\":\"go\"}");
    rq.history = &kExpectedRevID;
    rq.historyCount = 1;
    size_t commonAncestorIndex;
    doc = c4doc_put(db, &rq, &commonAncestorIndex, &error);
    REQUIRE(doc != nullptr);
    REQUIRE((unsigned long)commonAncestorIndex == 1ul);
    C4Slice kExpectedRev2ID = isRevTrees() ? C4STR("2-32c711b29ea3297e27f3c28c8b066a68e1bb3f7b")
                                           : C4STR("2@*");
    REQUIRE(doc->revID == kExpectedRev2ID);
    REQUIRE(doc->flags == (C4DocumentFlags)kExists);
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
    REQUIRE(doc->flags == (C4DocumentFlags)(kExists | kConflicted));
    if (isRevTrees()) {
        REQUIRE(doc->revID == kConflictRevID);
    } else {
        REQUIRE(doc->revID == kExpectedRev2ID);
    }

    c4doc_free(doc);
}
