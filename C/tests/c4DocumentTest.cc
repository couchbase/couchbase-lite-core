//
// c4DocumentTest.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Test.hh"  // IWYU pragma: keep
#include "c4Database.hh"
#include "c4Document+Fleece.h"
#include "c4Collection.h"
#include "fleece/Fleece.hh"

using namespace fleece;

TEST_CASE("Generate docID", "[Database][C]") {
    char buf[kC4GeneratedIDLength + 1];
    CHECK(c4doc_generateID(buf, 0) == nullptr);
    CHECK(c4doc_generateID(buf, kC4GeneratedIDLength) == nullptr);
    for ( int pass = 0; pass < 10; ++pass ) {
        REQUIRE(c4doc_generateID(buf, sizeof(buf)) == buf);
        C4Log("docID = '%s'", buf);
        REQUIRE(strlen(buf) == kC4GeneratedIDLength);
        CHECK(buf[0] == '~');
        for ( int i = 1; i < strlen(buf); ++i ) {
            bool validChar = isalpha(buf[i]) || isdigit(buf[i]) || buf[i] == '_' || buf[i] == '-';
            CHECK(validChar);
        }
    }
}

N_WAY_TEST_CASE_METHOD(C4Test, "Invalid docID", "[Document][C]") {
    c4log_warnOnErrors(false);
    TransactionHelper t(db);

    auto checkPutBadDocID = [this](C4Slice docID) {
        C4Error         error;
        C4DocPutRequest rq = {};
        rq.body            = C4Test::kFleeceBody;
        rq.save            = true;
        rq.docID           = docID;
        ExpectingExceptions x;
        auto                defaultColl = getCollection(db, kC4DefaultCollectionSpec);
        CHECK(c4coll_putDoc(defaultColl, &rq, nullptr, &error) == nullptr);
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorBadDocID);
    };

    SECTION("empty") { checkPutBadDocID(C4STR("")); }
    SECTION("too long") {
        char buf[241];
        memset(buf, 'x', sizeof(buf));
        checkPutBadDocID({buf, sizeof(buf)});
    }
    SECTION("bad UTF-8") { checkPutBadDocID(C4STR("oops\x00oops")); }
    SECTION("control character") { checkPutBadDocID(C4STR("oops\noops")); }
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

    auto defaultColl = c4db_getDefaultCollection(db,nullptr);
	C4Document *doc = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, ERROR_INFO());
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


N_WAY_TEST_CASE_METHOD(C4Test, "Document Get With Invalid ID", "[Document][C]") {
    ExpectingExceptions x;
    C4Error             error       = {};
    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    CHECK(c4coll_getDoc(defaultColl, nullslice, true, kDocGetCurrentRev, &error) == nullptr);
    CHECK(error == (C4Error{LiteCoreDomain, kC4ErrorBadDocID}));

    error = {};
    CHECK(c4coll_getDoc(defaultColl, ""_sl, true, kDocGetCurrentRev, &error) == nullptr);
    CHECK(error == (C4Error{LiteCoreDomain, kC4ErrorBadDocID}));

    error = {};
    std::string tooLong(300, 'x');
    CHECK(c4coll_getDoc(defaultColl, slice(tooLong), true, kDocGetCurrentRev, &error) == nullptr);
    CHECK(error == (C4Error{LiteCoreDomain, kC4ErrorBadDocID}));
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document CreateVersionedDoc", "[Document][C]") {
    // Try reading doc with mustExist=true, which should fail:
    C4Error     error;
    C4Document* doc;
    auto        defaultColl = c4db_getDefaultCollection(db, nullptr);
    doc                     = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, &error);
    REQUIRE(!doc);
    REQUIRE((uint32_t)error.domain == (uint32_t)LiteCoreDomain);
    REQUIRE(error.code == (int)kC4ErrorNotFound);
    c4doc_release(doc);

    // Test c4coll_getDoc, which also fails:
    for ( C4DocContentLevel content : {kDocGetMetadata, kDocGetCurrentRev, kDocGetAll} ) {
        doc = c4coll_getDoc(defaultColl, kDocID, true, content, &error);
        REQUIRE(!doc);
        REQUIRE((uint32_t)error.domain == (uint32_t)LiteCoreDomain);
        REQUIRE(error.code == (int)kC4ErrorNotFound);
    }

    // Now get the doc with mustExist=false, which returns an empty doc:
    doc = c4coll_getDoc(defaultColl, kDocID, false, kDocGetCurrentRev, ERROR_INFO(error));
    REQUIRE(doc != nullptr);
    REQUIRE(doc->flags == 0);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID.buf == 0);
    REQUIRE(doc->selectedRev.revID.buf == 0);
    c4doc_release(doc);

    {
        TransactionHelper t(db);
        C4DocPutRequest   rq = {};
        rq.revFlags          = kRevKeepBody;
        rq.existingRevision  = true;
        rq.docID             = kDocID;
        rq.history           = &kRevID;
        rq.historyCount      = 1;
        rq.body              = kFleeceBody;
        rq.save              = true;
        doc                  = c4coll_putDoc(defaultColl, &rq, nullptr, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        CHECK(doc->revID == kRevID);
        CHECK(doc->selectedRev.revID == kRevID);
        if ( isRevTrees() ) CHECK(doc->selectedRev.flags == (kRevKeepBody | kRevLeaf));
        else
            CHECK(doc->selectedRev.flags == (kRevLeaf));
        CHECK(docBodyEquals(doc, kFleeceBody));
        c4doc_release(doc);
    }

    // Reload the doc:
    doc = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, ERROR_INFO(error));
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
    doc = c4coll_getDocBySequence(defaultColl, 1, ERROR_INFO(error));
    REQUIRE(doc != nullptr);
    CHECK(doc->sequence == 1);
    CHECK(doc->flags == kDocExists);
    CHECK(doc->docID == kDocID);
    CHECK(doc->revID == kRevID);
    CHECK(doc->selectedRev.revID == kRevID);
    CHECK(doc->selectedRev.sequence == 1);
    CHECK(docBodyEquals(doc, kFleeceBody));
    c4doc_release(doc);

    // Get a bogus sequence
    doc = c4coll_getDocBySequence(defaultColl, 2, &error);
    CHECK(doc == nullptr);
    CHECK(error == C4Error{LiteCoreDomain, kC4ErrorNotFound});

    // Test c4coll_getDoc:
    for ( C4DocContentLevel content : {kDocGetMetadata, kDocGetCurrentRev, kDocGetAll} ) {
        doc = c4coll_getDoc(defaultColl, kDocID, true, content, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        CHECK(doc->sequence == 1);
        CHECK(doc->flags == kDocExists);
        CHECK(doc->docID == kDocID);
        CHECK(doc->revID == kRevID);
        CHECK(doc->selectedRev.revID == kRevID);
        CHECK(doc->selectedRev.sequence == 1);
        if ( content == kDocGetMetadata ) {
            CHECK(c4doc_getRevisionBody(doc) == nullslice);
            CHECK(c4doc_getProperties(doc) == nullptr);
        } else {
            CHECK(c4doc_getRevisionBody(doc) == kFleeceBody);
            CHECK(docBodyEquals(doc, kFleeceBody));
        }
        c4doc_release(doc);
    }
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document CreateMultipleRevisions", "[Document][C]") {
    const auto kFleeceBody2 = json2fleece("{'ok':'go'}");
    const auto kFleeceBody3 = json2fleece("{'ubu':'roi'}");
    createRev(kDocID, kRevID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody2, kRevKeepBody);
    createRev(kDocID, kRev2ID, kFleeceBody2);  // test redundant insert

    // Reload the doc:
    C4Error     error;
    auto        defaultColl = c4db_getDefaultCollection(db, nullptr);
    C4Document* doc         = c4coll_getDoc(defaultColl, kDocID, true, kDocGetAll, ERROR_INFO(error));
    REQUIRE(doc != nullptr);
    CHECK(doc->flags == kDocExists);
    CHECK(doc->docID == kDocID);
    CHECK(doc->revID == kRev2ID);
    CHECK(doc->selectedRev.revID == kRev2ID);
    CHECK(doc->selectedRev.sequence == (C4SequenceNumber)2);
    CHECK(docBodyEquals(doc, kFleeceBody2));

    if ( isRevTrees() ) {
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

        doc = c4coll_getDoc(defaultColl, kDocID, true, kDocGetAll, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        REQUIRE(c4doc_selectParentRevision(doc));
        CHECK(doc->selectedRev.revID == kRev2ID);
        CHECK(doc->selectedRev.sequence == (C4SequenceNumber)2);
        CHECK(doc->selectedRev.flags == kRevKeepBody);
        CHECK(docBodyEquals(doc, kFleeceBody2));
        c4doc_release(doc);

        // Test c4coll_getDoc:
        for ( C4DocContentLevel content : {kDocGetMetadata, kDocGetCurrentRev, kDocGetAll} ) {
            doc = c4coll_getDoc(defaultColl, kDocID, true, content, ERROR_INFO(error));
            REQUIRE(doc != nullptr);
            CHECK(doc->sequence == 3);
            CHECK(doc->flags == kDocExists);
            CHECK(doc->docID == kDocID);
            CHECK(doc->revID == kRev3ID);
            CHECK(doc->selectedRev.revID == kRev3ID);
            CHECK(doc->selectedRev.sequence == 3);
            if ( content == kDocGetMetadata ) CHECK(c4doc_getProperties(doc) == nullptr);
            else
                CHECK(docBodyEquals(doc, kFleeceBody3));
            c4doc_release(doc);
        }

        // Purge doc
        {
            TransactionHelper t(db);

            doc = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, ERROR_INFO(error));
            REQUIRE(doc);
            int nPurged = c4doc_purgeRevision(doc, {}, ERROR_INFO(error));
            CHECK(nPurged == 3);
            REQUIRE(c4doc_save(doc, 20, WITH_ERROR(&error)));
            c4doc_release(doc);
        }

        // Make sure it's gone:
        doc = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, &error);
        CHECK(!doc);
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorNotFound);
    } else {
        // The history is going to end with this database's SourceID, a random 64-bit hex string,
        // so we don't know exactly what it will be. But it will start "2@".
        alloc_slice history = c4doc_getRevisionHistory(doc, 99, nullptr, 0);
        CHECK(history.hasPrefix("2@"));
        CHECK(history.size <= 2 + 32);
    }
    c4doc_release(doc);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Purge", "[Database][Document][C]") {
    C4Error    err;
    const auto kFleeceBody2 = json2fleece("{'ok':'go'}");
    const auto kFleeceBody3 = json2fleece("{'ubu':'roi'}");
    createRev(kDocID, kRevID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody2);
    createRev(kDocID, kRev3ID, kFleeceBody3);

    C4DocPutRequest rq         = {};
    C4Slice         history[3] = {C4STR("3-ababab"), kRev2ID};
    if ( isRevTrees() ) {
        // Create a conflict
        rq.existingRevision = true;
        rq.docID            = kDocID;
        rq.history          = history;
        rq.historyCount     = 2;
        rq.allowConflict    = true;
        rq.body             = kFleeceBody3;
        rq.save             = true;
        REQUIRE(c4db_beginTransaction(db, WITH_ERROR(&err)));
        auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
        auto doc         = c4coll_putDoc(defaultColl, &rq, nullptr, ERROR_INFO(err));
        REQUIRE(doc);
        c4doc_release(doc);
        REQUIRE(c4db_endTransaction(db, true, WITH_ERROR(&err)));
    }

    REQUIRE(c4db_beginTransaction(db, WITH_ERROR(&err)));
    auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    REQUIRE(c4coll_purgeDoc(defaultColl, kDocID, WITH_ERROR(&err)));
    REQUIRE(c4db_endTransaction(db, true, WITH_ERROR(&err)));

    REQUIRE(c4coll_getDocumentCount(defaultColl) == 0);

    if ( isRevTrees() ) {
        // c4doc_purgeRevision is not available with version vectors
        createRev(kDocID, kRevID, kFleeceBody);
        createRev(kDocID, kRev2ID, kFleeceBody2);
        createRev(kDocID, kRev3ID, kFleeceBody3);
        REQUIRE(c4db_beginTransaction(db, WITH_ERROR(&err)));
        auto doc = c4coll_putDoc(defaultColl, &rq, nullptr, ERROR_INFO(err));
        REQUIRE(doc);
        REQUIRE(c4db_endTransaction(db, true, WITH_ERROR(&err)));
        c4doc_release(doc);

        REQUIRE(c4db_beginTransaction(db, WITH_ERROR(&err)));
        doc = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, ERROR_INFO(err));
        REQUIRE(doc);
        CHECK(c4doc_purgeRevision(doc, kRev2ID, WITH_ERROR(&err)) == 0);
        REQUIRE(c4doc_purgeRevision(doc, kC4SliceNull, WITH_ERROR(&err)) == 4);
        REQUIRE(c4doc_save(doc, 20, WITH_ERROR(&err)));
        c4doc_release(doc);
        REQUIRE(c4db_endTransaction(db, true, WITH_ERROR(&err)));
        REQUIRE(c4coll_getDocumentCount(defaultColl) == 0);
    }
}


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
    C4Error     error;
    C4Document* doc;
    size_t      commonAncestorIndex;
    alloc_slice revID;

    TransactionHelper t(db);

    C4Slice kExpectedRevID, kExpectedRev2ID, kConflictRevID;
    if ( isRevTrees() ) {
        kExpectedRevID = C4STR("1-feb9f18cfe84f614f750040e3eed3eb84a6b5332");
    } else {
        kExpectedRevID = C4STR("1@*");
    }

    C4DocPutRequest rqTemplate = {};
    rqTemplate.docID           = kDocID;
    rqTemplate.body            = kFleeceBody;
    rqTemplate.save            = true;

    // Creating doc given ID:
    {
        C4DocPutRequest rq = rqTemplate;
        rq.docID           = kDocID;
        rq.body            = kFleeceBody;
        rq.save            = true;
        auto defaultColl   = getCollection(db, kC4DefaultCollectionSpec);
        doc                = c4coll_putDoc(defaultColl, &rq, nullptr, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        REQUIRE(doc->docID == kDocID);

        CHECK(doc->revID == kExpectedRevID);
        CHECK(doc->flags == kDocExists);
        CHECK(doc->selectedRev.revID == kExpectedRevID);
        c4doc_release(doc);
    }

    // Update doc:
    {
        auto            body = json2fleece("{'ok':'go'}");
        C4DocPutRequest rq   = rqTemplate;
        rq.body              = body;
        rq.history           = &kExpectedRevID;
        rq.historyCount      = 1;
        auto defaultColl     = getCollection(db, kC4DefaultCollectionSpec);
        doc                  = c4coll_putDoc(defaultColl, &rq, &commonAncestorIndex, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        CHECK((unsigned long)commonAncestorIndex == 0ul);
        if ( isRevTrees() ) {
            kExpectedRev2ID = C4STR("2-134f93aab57c91b159373e97764ef82a5eb058a0");
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
        C4DocPutRequest rq   = rqTemplate;
        auto            body = json2fleece("{'from':'elsewhere'}");
        rq.body              = body;
        rq.existingRevision  = true;
        rq.remoteDBID        = 1;
        if ( isRevTrees() ) {
            kConflictRevID = C4STR("2-deadbeef");
        } else {
            kConflictRevID = C4STR("1@CarolCarolCarolCarolCA");
        }

        C4Slice history[2] = {kConflictRevID, kExpectedRevID};
        rq.history         = history;
        rq.historyCount    = 2;
        rq.allowConflict   = true;
        auto defaultColl   = getCollection(db, kC4DefaultCollectionSpec);
        doc                = c4coll_putDoc(defaultColl, &rq, &commonAncestorIndex, ERROR_INFO(error));

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
        rq.body            = nullslice;
        rq.revFlags        = kRevDeleted;
        rq.history         = &kExpectedRev2ID;
        rq.historyCount    = 1;
        auto defaultColl   = getCollection(db, kC4DefaultCollectionSpec);
        doc                = c4coll_putDoc(defaultColl, &rq, &commonAncestorIndex, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        CHECK(doc->flags == (kDocExists | kDocDeleted | kDocConflicted));
        revID = doc->revID;
        c4doc_release(doc);
    }

    // Resurrect it:
    {
        C4DocPutRequest rq   = rqTemplate;
        auto            body = json2fleece("{'ok':'again'}");
        rq.body              = body;
        rq.history           = (C4Slice*)&revID;
        rq.historyCount      = 1;
        auto defaultColl     = getCollection(db, kC4DefaultCollectionSpec);
        doc                  = c4coll_putDoc(defaultColl, &rq, &commonAncestorIndex, ERROR_INFO(error));
        REQUIRE(doc != nullptr);
        CHECK(doc->flags == (kDocExists | kDocConflicted));
        c4doc_release(doc);
    }
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document create from existing rev", "[Document][C]") {
    C4Error           error;
    TransactionHelper t(db);

    // Creating doc given ID:
    C4DocPutRequest rq    = {};
    rq.docID              = kDocID;
    rq.body               = kFleeceBody;
    rq.existingRevision   = true;
    C4String history[1]   = {kRevID};
    rq.history            = history;
    rq.historyCount       = 1;
    rq.save               = true;
    size_t commonAncestor = 9999;
    auto   defaultColl    = getCollection(db, kC4DefaultCollectionSpec);
    auto   doc            = c4coll_putDoc(defaultColl, &rq, &commonAncestor, ERROR_INFO(error));
    REQUIRE(doc != nullptr);
    CHECK(commonAncestor == 1);
    CHECK(doc->docID == kDocID);
    CHECK(doc->revID == kRevID);
    c4doc_release(doc);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Update", "[Document][C]") {
    C4Log("Begin test");
    C4Error     error;
    C4Document* doc;
    auto        defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    {
        C4Log("Begin create");
        TransactionHelper t(db);
        doc = c4coll_createDoc(defaultColl, kDocID, kFleeceBody, 0, ERROR_INFO(error));
        REQUIRE(doc);
    }
    C4Log("After save");
    C4Slice kExpectedRevID;
    if ( isRevTrees() ) {
        kExpectedRevID = C4STR("1-feb9f18cfe84f614f750040e3eed3eb84a6b5332");
    } else {
        kExpectedRevID = C4STR("1@*");
    }

    CHECK(doc->revID == kExpectedRevID);
    CHECK(doc->flags == kDocExists);
    CHECK(doc->selectedRev.revID == kExpectedRevID);
    CHECK(doc->docID == kDocID);

    // Read the doc into another C4Document:
    auto doc2 = c4coll_getDoc(defaultColl, kDocID, false, kDocGetAll, ERROR_INFO(error));
    REQUIRE(doc2->revID == kExpectedRevID);

    // Update it a few times:
    for ( int update = 2; update <= 5; ++update ) {
        C4Log("Begin save #%d", update);
        TransactionHelper   t(db);
        fleece::alloc_slice oldRevID(doc->revID);
        auto                updatedDoc = c4doc_update(doc, json2fleece("{'ok':'go'}"), 0, ERROR_INFO(error));
        REQUIRE(updatedDoc);
        CHECK(updatedDoc != doc);
        CHECK(doc->selectedRev.revID == oldRevID);
        CHECK(doc->revID == oldRevID);
        c4doc_release(doc);
        doc = updatedDoc;
    }
    C4Log("After multiple updates");
    C4Slice kExpectedRev2ID;
    if ( isRevTrees() ) {
        kExpectedRev2ID = C4STR("5-a9732d3d5c6aa5637049721a6f21eb9c97b831d0");
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
        REQUIRE(c4coll_createDoc(defaultColl, kDocID, json2fleece("{'ok':'no way'}"), 0, &error) == nullptr);
        CHECK(error == C4Error{LiteCoreDomain, kC4ErrorConflict});
    }

    c4doc_release(doc);
    c4doc_release(doc2);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Delete then Update", "[Document][C]") {
    TransactionHelper t(db);
    auto              defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    // Create a doc:
    C4Error error;
    auto    doc = c4coll_createDoc(defaultColl, kDocID, kFleeceBody, 0, ERROR_INFO(error));
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

N_WAY_TEST_CASE_METHOD(C4Test, "LoadRevisions After Purge", "[Document][C]") {
    TransactionHelper t(db);
    auto              defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    for ( auto content = int(kDocGetMetadata); content <= int(kDocGetAll); ++content ) {
        C4Log("---- Content level %d", content);

        // Create document
        c4::ref<C4Document> fullDoc = c4coll_createDoc(defaultColl, kDocID, kFleeceBody, 0, ERROR_INFO());
        REQUIRE(fullDoc);

        // Get the document, with the current content level
        c4::ref<C4Document> curDoc = c4coll_getDoc(defaultColl, kDocID, true, C4DocContentLevel(content), ERROR_INFO());

        // Purge the doc on disk!
        REQUIRE(c4coll_purgeDoc(defaultColl, kDocID, ERROR_INFO()));

        ExpectingExceptions x;
        C4Error             error;

        // If the doc hasn't loaded the revision history/tree yet, it will try to and fail because
        // its sequence doesn't exist anymore; it will report this as a Conflict error.
        // At the `kDocGetAll` level, the document will get all the way to saving and find that the
        // document no longer exists, so it will report NotFound.
        C4Error expectedError = {LiteCoreDomain, (content < kDocGetAll ? kC4ErrorConflict : kC4ErrorNotFound)};

        // Try to set curDoc's remote ancestor:
        if ( content < kDocGetAll ) {
            CHECK(!c4doc_setRemoteAncestor(curDoc, 1, curDoc->revID, &error));
            CHECK(error == expectedError);
        }

        // Try to resolve a conflict:
        error = {};
        CHECK(!c4doc_resolveConflict(curDoc, curDoc->revID, kRev4ID, nullslice, {}, &error));
        CHECK(error == expectedError);

        // Try to update the document:
        error = {};
        CHECK(c4doc_update(curDoc, kFleeceBody, {}, &error) == nullptr);
        CHECK(error == (C4Error{LiteCoreDomain, kC4ErrorNotFound}));
    }
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Body Doesn't Change", "[Document][C]") {
    // CBL-2033
    TransactionHelper t(db);
    // Create document
    createRev(kDocID, kRevID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody);

    // Get the document, with only the current revision:
    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    c4::ref<C4Document> curDoc      = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, ERROR_INFO());
    FLDict              properties  = c4doc_getProperties(curDoc);
    slice               curBody     = c4doc_getRevisionBody(curDoc);

    // Force the rest of the doc's data to be loaded:
    REQUIRE(c4doc_setRemoteAncestor(curDoc, 1, kRevID, ERROR_INFO()));

    // Check this didn't replace the current rev's body with a new alloc_slice:
    REQUIRE(c4doc_selectCurrentRevision(curDoc));
    FLDict newProperties = c4doc_getProperties(curDoc);
    CHECK(newProperties == properties);
    slice newCurBody = c4doc_getRevisionBody(curDoc);
    CHECK(newCurBody == curBody);
    CHECK(newCurBody.buf == curBody.buf);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Deletion External Pointers", "[Document][C]") {
    // CBL-2033
    const slice streetVal = "1 Main street";

    {
        TransactionHelper t(db);
        auto              enc = c4db_getSharedFleeceEncoder(db);
        FLEncoder_BeginDict(enc, 1);
        FLEncoder_WriteKey(enc, FLSTR("address"));
        FLEncoder_BeginDict(enc, 1);
        FLEncoder_WriteKey(enc, FLSTR("street"));
        FLEncoder_WriteString(enc, streetVal);
        FLEncoder_EndDict(enc);
        FLEncoder_EndDict(enc);

        FLError err;
        auto    result = FLEncoder_Finish(enc, &err);
        FLEncoder_Reset(enc);
        createRev(kDocID, kRevID, (C4Slice)result);
        FLSliceResult_Release(result);
    }

    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    c4::ref<C4Document> doc         = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc);
    FLValue docBodyVal = FLValue_FromData(c4doc_getRevisionBody(doc), kFLTrusted);
    REQUIRE(docBodyVal);
    FLDict docBody = FLValue_AsDict(docBodyVal);
    REQUIRE(docBody);
    FLDict address = FLValue_AsDict(FLDict_Get(docBody, FLSTR("address")));
    REQUIRE(address);

    slice street = FLValue_AsString(FLDict_Get(address, FLSTR("street")));
    CHECK(street == streetVal);

    {
        TransactionHelper   t(db);
        c4::ref<C4Document> updated = c4doc_update(doc, kC4SliceNull, kRevDeleted, nullptr);
    }

    CHECK(street == streetVal);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Redundant VV Merge", "[Document][RevIDs][Conflict][C]") {
    if ( isRevTrees() ) return;

    const auto kFleeceBody2 = json2fleece("{'ok':'go'}");
    auto       defaultColl  = getCollection(db, kC4DefaultCollectionSpec);
    createRev(kDocID, "8@*, 7@AliceAliceAliceAliceAA, 6@BobBobBobBobBobBobBobA; 4@CarolCarolCarolCarolCA"_sl,
              kFleeceBody2);

    // "Pull" a revision that merges the same conflict with the same body but different revID:
    TransactionHelper t(db);
    C4Slice           otherRev =
            "9@ZegpoldZegpoldZegpoldA, 7@AliceAliceAliceAliceAA, 6@BobBobBobBobBobBobBobA; 4@CarolCarolCarolCarolCA"_sl;
    C4DocPutRequest rq{
            .body             = kFleeceBody2,
            .docID            = kDocID,
            .revFlags         = kRevKeepBody,
            .existingRevision = true,
            .allowConflict    = true,
            .history          = &otherRev,
            .historyCount     = 1,
            .save             = true,
            .remoteDBID       = 1,
    };
    c4::ref<C4Document> doc = c4coll_putDoc(defaultColl, &rq, nullptr, ERROR_INFO());
    REQUIRE(doc);

    CHECK(doc->revID == "8@*"_sl);
    CHECK(fleece::Dict(c4doc_getProperties(doc)).toJSONString() == R"({"ok":"go"})");
    CHECK(alloc_slice(c4doc_getRemoteAncestor(doc, C4RemoteID(1))) == "9@ZegpoldZegpoldZegpoldA"_sl);
    REQUIRE(c4doc_selectRevision(doc, "9@ZegpoldZegpoldZegpoldA"_sl, true, WITH_ERROR()));
    CHECK(fleece::Dict(c4doc_getProperties(doc)).toJSONString() == R"({"ok":"go"})");
    CHECK(doc->sequence == 1);  // adding remote rev didn't disturb local sequence
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Conflict", "[Document][RevIDs][Conflict][C]") {
    C4Error err;
    slice   kRev1ID, kRev2ID, kRev3ID, kRev3ConflictID, kRev4ConflictID;
    if ( isRevTrees() ) {
        kRev1ID         = "1-aaaaaa";
        kRev2ID         = "2-aaaaaa";
        kRev3ID         = "3-aaaaaa";
        kRev3ConflictID = "3-ababab";
        kRev4ConflictID = "4-dddd";
    } else {
        kRev1ID         = "1@*";
        kRev2ID         = "2@*";
        kRev3ID         = "3@*";
        kRev3ConflictID = "3@CarolCarolCarolCarolCA";
        kRev4ConflictID = "4@CarolCarolCarolCarolCA";
    }

    auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);

    const auto kFleeceBody2 = json2fleece("{'ok':'go'}");
    const auto kFleeceBody3 = json2fleece("{'ubu':'roi'}");
    const auto kFleeceBody4 = json2fleece("{'four':'four'}");

    createRev(kDocID, kRev1ID, kFleeceBody);
    createRev(kDocID, kRev2ID, kFleeceBody2, kRevKeepBody);
    createRev(kDocID, kRev3ID, kFleeceBody3);

    TransactionHelper t(db);

    {
        // "Pull" a conflicting revision:
        C4Slice         history[3] = {kRev4ConflictID, kRev3ConflictID, kRev2ID};
        C4DocPutRequest rq         = {};
        rq.existingRevision        = true;
        rq.docID                   = kDocID;
        rq.history                 = history;
        rq.historyCount            = 3;
        rq.allowConflict           = true;
        rq.body                    = kFleeceBody4;
        rq.revFlags                = kRevKeepBody;
        rq.save                    = true;
        rq.remoteDBID              = 1;
        c4::ref<C4Document> doc    = c4coll_putDoc(defaultColl, &rq, nullptr, ERROR_INFO(err));
        REQUIRE(doc);
        CHECK(doc->selectedRev.revID == kRev4ConflictID);

        // Check that the local revision is still current:
        CHECK(doc->revID == kRev3ID);
        REQUIRE(c4doc_selectCurrentRevision(doc));
        CHECK(doc->selectedRev.revID == kRev3ID);
        CHECK((int)doc->selectedRev.flags == kRevLeaf);

        if ( isRevTrees() ) {
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
        c4::ref<C4Document> doc = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, ERROR_INFO(err));
        REQUIRE(c4doc_resolveConflict(doc, kRev4ConflictID, kRev3ID, nullslice, 0, WITH_ERROR(&err)));
        c4doc_selectCurrentRevision(doc);
        CHECK(docBodyEquals(doc, kFleeceBody4));
        if ( isRevTrees() ) {
            // kRevID -- kRev2ID -- kRev3ConflictID -- kMergedRevID
            CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevKeepBody));
            CHECK(doc->selectedRev.revID == kRev4ConflictID);
            alloc_slice revHistory(c4doc_getRevisionHistory(doc, 99, nullptr, 0));
            CHECK(revHistory == "4-dddd,3-ababab,2-aaaaaa,1-aaaaaa"_sl);

            CHECK(c4doc_selectParentRevision(doc));
            CHECK(doc->selectedRev.revID == kRev3ConflictID);
            CHECK((int)doc->selectedRev.flags == 0);

            CHECK(c4doc_selectParentRevision(doc));
            CHECK(doc->selectedRev.revID == kRev2ID);
            CHECK((int)doc->selectedRev.flags == 0);
        } else {
            CHECK((int)doc->selectedRev.flags == kRevLeaf);
            CHECK(doc->selectedRev.revID == "4@CarolCarolCarolCarolCA"_sl);
            alloc_slice vector(c4doc_getRevisionHistory(doc, 0, nullptr, 0));
            CHECK(vector == "4@CarolCarolCarolCarolCA; 2@*"_sl);
        }

        CHECK(c4doc_selectRevision(doc, kRev4ConflictID, false, nullptr));
        CHECK(!c4doc_selectRevision(doc, kRev3ID, false, nullptr));
    }

    if ( !isRevTrees() ) {
        C4Log("--- Resolve, remote wins but merge vectors");
        // We have to update the local revision to get into this state.
        // Note we are NOT saving the doc, so we don't mess up the following test block.
        slice           kSomeoneElsesVersion = "7@AliceAliceAliceAliceAA";
        C4Slice         history[]            = {kSomeoneElsesVersion, kRev3ID};
        C4DocPutRequest rq                   = {};
        rq.existingRevision                  = true;
        rq.docID                             = kDocID;
        rq.history                           = history;
        rq.historyCount                      = 2;
        rq.body                              = kFleeceBody2;

        c4::ref<C4Document> doc = c4coll_putDoc(defaultColl, &rq, nullptr, ERROR_INFO(err));
        REQUIRE(doc);

        REQUIRE(c4doc_resolveConflict(doc, kRev4ConflictID, kSomeoneElsesVersion, nullslice, 0, WITH_ERROR(&err)));
        c4doc_selectCurrentRevision(doc);
        CHECK(docBodyEquals(doc, kFleeceBody4));

        CHECK((int)doc->selectedRev.flags == kRevLeaf);
        CHECK(doc->selectedRev.revID == "8@*"_sl);
        alloc_slice vector(c4doc_getRevisionHistory(doc, 0, nullptr, 0));
        CHECK(vector == "8@*, 7@AliceAliceAliceAliceAA, 4@CarolCarolCarolCarolCA;"_sl);
        CHECK(c4doc_selectRevision(doc, kRev4ConflictID, false, nullptr));
        CHECK(!c4doc_selectRevision(doc, kRev3ID, false, nullptr));
    }

    {
        C4Log("--- Merge onto remote");

        c4::ref<C4Document> doc = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, ERROR_INFO(err));
        REQUIRE(c4doc_resolveConflict(doc, kRev4ConflictID, kRev3ID, mergedBody, 0, WITH_ERROR(&err)));
        c4doc_selectCurrentRevision(doc);
        CHECK(docBodyEquals(doc, mergedBody));
        if ( isRevTrees() ) {
            // kRevID -- kRev2ID -- kRev3ConflictID -- [kRev4ConflictID] -- kMergedRevID
            CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevNew));
            CHECK(doc->selectedRev.revID == "5-940fe7e020dbf8db0f82a5d764870c4b6c88ae99"_sl);
            alloc_slice revHistory(c4doc_getRevisionHistory(doc, 99, nullptr, 0));
            CHECK(revHistory == "5-940fe7e020dbf8db0f82a5d764870c4b6c88ae99,4-dddd,3-ababab,2-aaaaaa,1-aaaaaa"_sl);

            CHECK(c4doc_selectParentRevision(doc));
            CHECK(doc->selectedRev.revID == kRev4ConflictID);
            CHECK((int)doc->selectedRev.flags == kRevKeepBody);

            CHECK(c4doc_selectParentRevision(doc));
            CHECK(doc->selectedRev.revID == kRev3ConflictID);
            CHECK((int)doc->selectedRev.flags == 0);

            CHECK(c4doc_selectParentRevision(doc));
            CHECK(doc->selectedRev.revID == kRev2ID);
            CHECK((int)doc->selectedRev.flags == 0);
        } else {
            CHECK((int)doc->selectedRev.flags == kRevLeaf);
            CHECK(doc->selectedRev.revID == "9@*"_sl);
            alloc_slice vector(c4doc_getRevisionHistory(doc, 0, nullptr, 0));
            CHECK(vector == "9@*, 4@CarolCarolCarolCarolCA, 3@*;"_sl);
        }

        CHECK(!c4doc_selectRevision(doc, kRev3ID, false, nullptr));
    }

    {
        C4Log("--- Resolve, local wins");
        c4::ref<C4Document> doc = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, ERROR_INFO(err));
        REQUIRE(doc);
        REQUIRE(c4doc_resolveConflict(doc, kRev3ID, kRev4ConflictID, nullslice, 0, WITH_ERROR(&err)));
        // kRevID -- [kRev2ID] -- kRev3ID
        c4doc_selectCurrentRevision(doc);
        CHECK(docBodyEquals(doc, kFleeceBody3));
        if ( isRevTrees() ) {
            CHECK((int)doc->selectedRev.flags == kRevLeaf);
            CHECK(doc->selectedRev.revID == kRev3ID);

            CHECK(c4doc_selectParentRevision(doc));
            CHECK(doc->selectedRev.revID == kRev2ID);
            CHECK((int)doc->selectedRev.flags == kRevKeepBody);

            CHECK(!c4doc_selectRevision(doc, kRev4ConflictID, false, nullptr));
            CHECK(!c4doc_selectRevision(doc, kRev3ConflictID, false, nullptr));
        } else {
            CHECK((int)doc->selectedRev.flags == kRevLeaf);
            CHECK(doc->selectedRev.revID == "a@*"_sl);
            alloc_slice vector(c4doc_getRevisionHistory(doc, 0, nullptr, 0));
            CHECK(vector == "a@*, 4@CarolCarolCarolCarolCA, 3@*;"_sl);
        }
    }

    {
        C4Log("--- Merge onto local");
        c4::ref<C4Document> doc = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, ERROR_INFO(err));
        REQUIRE(doc);
        REQUIRE(c4doc_resolveConflict(doc, kRev3ID, kRev4ConflictID, mergedBody, 0, WITH_ERROR(&err)));
        // kRevID -- [kRev2ID] -- kRev3ID -- kMergedRevID
        c4doc_selectCurrentRevision(doc);
        CHECK(docBodyEquals(doc, mergedBody));
        if ( isRevTrees() ) {
            CHECK((int)doc->selectedRev.flags == (kRevLeaf | kRevNew));
            CHECK(doc->selectedRev.revID == "4-333ee0677b5f1e1e5064b050d417a31d2455dc30"_sl);

            CHECK(c4doc_selectParentRevision(doc));
            CHECK(doc->selectedRev.revID == kRev3ID);
            CHECK((int)doc->selectedRev.flags == 0);

            CHECK(c4doc_selectParentRevision(doc));
            CHECK(doc->selectedRev.revID == kRev2ID);
            CHECK((int)doc->selectedRev.flags == kRevKeepBody);

            CHECK(!c4doc_selectRevision(doc, kRev4ConflictID, false, nullptr));
            CHECK(!c4doc_selectRevision(doc, kRev3ConflictID, false, nullptr));
        } else {
            CHECK((int)doc->selectedRev.flags == kRevLeaf);
            CHECK(doc->selectedRev.revID == "b@*"_sl);
            alloc_slice vector(c4doc_getRevisionHistory(doc, 0, nullptr, 0));
            CHECK(vector == "b@*, 4@CarolCarolCarolCarolCA, 3@*;"_sl);
        }
    }
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document from Fleece", "[Document][C]") {
    if ( !isRevTrees() ) return;

    CHECK(c4doc_containingValue((FLValue)0x12345678) == nullptr);

    const auto kFleeceBody = json2fleece("{'ubu':'roi'}");
    createRev(kDocID, kRevID, kFleeceBody);

    auto        defaultColl = c4db_getDefaultCollection(db, nullptr);
    C4Document* doc         = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc);
    auto root = FLValue(c4doc_getProperties(doc));
    REQUIRE(root);
    CHECK(c4doc_containingValue(root) == doc);
    FLValue ubu = FLDict_Get(FLValue_AsDict(root), "ubu"_sl);
    CHECK(c4doc_containingValue(ubu) == doc);
    c4doc_release(doc);

    CHECK(c4doc_containingValue(root) == nullptr);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Leaf Document from Fleece", "[Document][C]") {
    if ( !isRevTrees() ) return;

    CHECK(c4doc_containingValue((FLValue)0x12345678) == nullptr);

    const auto kFleeceBody = json2fleece("{'ubu':'roi'}");
    createRev(kDocID, kRevID, kFleeceBody);

    auto        defaultColl = c4db_getDefaultCollection(db, nullptr);
    C4Document* doc         = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc);
    CHECK(doc->selectedRev.revID == kRevID);
    auto root = FLValue(c4doc_getProperties(doc));
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
    FLDict  d   = FLValue_AsDict(val);
    REQUIRE(d);

    FLDictKey testKey = FLDictKey_Init(C4STR("@type"));
    FLValue   testVal = FLDict_GetWithKey(d, &testKey);

    FLSlice blobSl = FLSTR("blob");  // Windows cannot compile this inside of a REQUIRE
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
    d   = FLValue_AsDict(val);
    REQUIRE(d);

    CHECK(!c4doc_dictContainsBlobs(d));
    FLDoc_Release(result);
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties 2", "[Document][C]") {
    // Check that old meta properties get removed:
    TransactionHelper t(db);
    auto              sk   = c4db_getFLSharedKeys(db);
    auto              dict = json2dict("{_id:'foo', _rev:'1-2345', x:17}");
    CHECK(c4doc_hasOldMetaProperties(dict));
    alloc_slice stripped = c4doc_encodeStrippingOldMetaProperties(dict, sk, nullptr);
    Doc         doc(stripped, kFLTrusted, sk);
    CHECK(fleece2json(stripped) == "{x:17}");
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties 3", "[Document][Blob][C]") {
    // Check that _attachments isn't removed if there are non-translated attachments in it,
    // but that the translated-from-blob attachments are removed:
    TransactionHelper t(db);
    auto              sk   = c4db_getFLSharedKeys(db);
    auto              dict = json2dict("{_attachments: {'blob_/foo/1': {'digest': 'sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU='},"
                                                    "oldie: {'digest': 'sha1-xVVVVVVVVVVVVVVVVVVVVVVVVVU='} },"
                                                    "foo: [ 0, {'@type':'blob', digest:'sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU='} ] }");
    CHECK(c4doc_hasOldMetaProperties(dict));
    alloc_slice stripped = c4doc_encodeStrippingOldMetaProperties(dict, sk, nullptr);
    Doc         doc(stripped, kFLTrusted, sk);
    CHECK(fleece2json(stripped)
          == "{_attachments:{oldie:{digest:\"sha1-xVVVVVVVVVVVVVVVVVVVVVVVVVU=\"}},foo:[0,{\"@type\":\"blob\",digest:"
             "\"sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU=\"}]}");
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties 4", "[Document][Blob][C]") {
    // Check that a translated attachment whose digest is different than its blob (i.e. the
    // attachment was probably modified by a non-blob-aware system) has its digest transferred to
    // the blob before being deleted. See #507. (Also, the _attachments property should be deleted.)
    TransactionHelper t(db);
    auto              sk   = c4db_getFLSharedKeys(db);
    auto              dict = json2dict(
            "{_attachments: {'blob_/foo/1': {'digest': "
                         "'sha1-XXXVVVVVVVVVVVVVVVVVVVVVVVU=',content_type:'image/png',revpos:23}},"
                         "foo: [ 0, {'@type':'blob', digest:'sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU=',content_type:'text/plain'} ] }");
    CHECK(c4doc_hasOldMetaProperties(dict));
    alloc_slice stripped = c4doc_encodeStrippingOldMetaProperties(dict, sk, nullptr);
    Doc         doc(stripped, kFLTrusted, sk);
    CHECK(fleece2json(stripped)
          == "{foo:[0,{\"@type\":\"blob\",content_type:\"image/png\",digest:\"sha1-XXXVVVVVVVVVVVVVVVVVVVVVVVU=\"}]}");
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Legacy Properties 5", "[Document][Blob][C]") {
    // Check that the 2.0.0 blob_<number> gets removed:
    TransactionHelper t(db);
    auto              sk   = c4db_getFLSharedKeys(db);
    auto              dict = json2dict(
            "{_attachments: {'blob_1': {'digest': "
                         "'sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU=',content_type:'image/png',revpos:23}},"
                         "foo: [ 0, {'@type':'blob', digest:'sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU=',content_type:'text/plain'} ] }");
    CHECK(c4doc_hasOldMetaProperties(dict));
    alloc_slice stripped = c4doc_encodeStrippingOldMetaProperties(dict, sk, nullptr);
    Doc         doc(stripped, kFLTrusted, sk);
    CHECK(fleece2json(stripped)
          == "{foo:[0,{\"@type\":\"blob\",content_type:\"text/plain\",digest:\"sha1-VVVVVVVVVVVVVVVVVVVVVVVVVVU=\"}]}");
}

N_WAY_TEST_CASE_METHOD(C4Test, "Document Global Rev ID", "[Document][C]") {
    createRev(kDocID, kRevID, kFleeceBody);
    auto        defaultColl = c4db_getDefaultCollection(db, nullptr);
    C4Document* doc         = c4coll_getDoc(defaultColl, kDocID, true, kDocGetCurrentRev, nullptr);
    alloc_slice revID       = c4doc_getSelectedRevIDGlobalForm(doc);
    C4Log("Global rev ID = %.*s", (int)revID.size, (char*)revID.buf);
    CHECK(revID.findByte('*') == nullptr);
    c4doc_release(doc);
}
