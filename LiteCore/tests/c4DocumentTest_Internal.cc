//
// c4DocumentTest_Internal.cc
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Test.hh"
#include "c4Document+Fleece.h"
#include "c4Private.h"
#include "Benchmark.hh"
#include "fleece/Fleece.hh"
#include "SecureDigest.hh"
#include "slice_stream.hh"

using namespace fleece;

// These are C4 API tests that need internal functions not exported from the DLL,
// so this source file is linked into the C++Tests.


N_WAY_TEST_CASE_METHOD(C4Test, "Document FindDocAncestors", "[Document][C]") {
    C4String                  doc1 = C4STR("doc1"), doc2 = C4STR("doc2"), doc3 = C4STR("doc3");
    auto                      toString      = [](C4SliceResult sr) { return std::string(alloc_slice(std::move(sr))); };
    static constexpr bool     kNoBodies     = false;
    C4RemoteID                kRemoteID     = 1;
    static constexpr unsigned kMaxAncestors = 4;
    C4SliceResult             ancestors[4]  = {};

    auto findDocAncestor = [&](C4Slice docID, C4Slice revID, bool requireBodies = false) -> std::string {
        C4SliceResult ancestors[1] = {};
        REQUIRE(c4db_findDocAncestors(db, 1, kMaxAncestors, requireBodies, kRemoteID, &docID, &revID, ancestors,
                                      WITH_ERROR()));
        return toString(std::move(ancestors[0]));
    };

    if ( isRevTrees() ) {
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
        CHECK(findDocAncestor(doc1, kRev3ID) == "8");  // kRevHaveLocal | kRevSame

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
        C4String docIDs[4] = {doc2, doc1, C4STR("doc4"), doc3};
        C4String revIDs[4] = {"4-deadbeef"_sl, kRev3ID, C4STR("17-eeee"), "2-f000"_sl};
        REQUIRE(c4db_findDocAncestors(db, 4, kMaxAncestors, kNoBodies, kRemoteID, docIDs, revIDs, ancestors,
                                      WITH_ERROR()));
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
        CHECK(findDocAncestor(doc1, "3@100,10@8"_sl) == "0");  // kRevSame

        // Require bodies:
        CHECK(findDocAncestor(doc1, "3@100,10@8"_sl, true) == "0");  // kRevSame

        // Older revision:
        CHECK(findDocAncestor(doc1, "2@100,10@8"_sl) == "2");  // kRevNewer

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
        C4String docIDs[4] = {doc2, doc1, C4STR("doc4"), doc3};
        C4String revIDs[4] = {"9@100,10@8"_sl, "3@100,10@8"_sl, C4STR("17@17"), "1@99,3@300,30@8"_sl};
        REQUIRE(c4db_findDocAncestors(db, 4, kMaxAncestors, kNoBodies, kRemoteID, docIDs, revIDs, ancestors,
                                      WITH_ERROR()));
        CHECK(toString(std::move(ancestors[0])) == R"(1["3@100,10@8"])");
        CHECK(alloc_slice(std::move(ancestors[1])) == "0");
        CHECK(!slice(ancestors[2]));
        CHECK(toString(std::move(ancestors[3])) == R"(1["3@300,30@8"])");
    }
}

// Repro case for https://github.com/couchbase/couchbase-lite-core/issues/478
N_WAY_TEST_CASE_METHOD(C4Test, "Document Clobber Remote Rev", "[Document][C]") {
    if ( !isRevTrees() ) return;

    TransactionHelper t(db);

    // Write doc to db
    createRev(kDocID, kRevID, kFleeceBody);

    // Use default remote id
    C4RemoteID testRemoteId = 1;

    // Read doc from db and keep in memory
    C4Error error;
    auto    curDoc = c4db_getDoc(db, kDocID, false, kDocGetAll, ERROR_INFO(error));
    REQUIRE(curDoc != nullptr);

    // Call MarkRevSynced which will set the flag
    bool markSynced = c4db_markSynced(db, kDocID, curDoc->revID, curDoc->sequence, testRemoteId, ERROR_INFO(error));
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

static alloc_slice digest(slice body, slice parentRevID, bool deleted) {
    // Get SHA-1 digest of (length-prefixed) parent rev ID, deletion flag, and revision body:
    uint8_t        revLen  = (uint8_t)std::min((unsigned long)parentRevID.size, 255ul);
    uint8_t        delByte = deleted;
    litecore::SHA1 sha1
            = (litecore::SHA1Builder() << revLen << slice(parentRevID.buf, revLen) << delByte << body).finish();
    alloc_slice d = slice_ostream::alloced(100, [&sha1](slice_ostream& out) { return out.writeHex(sha1.asSlice()); });
    return d;
}

N_WAY_TEST_CASE_METHOD(C4Test, "Random RevID", "[Document][C]") {
    if ( !isRevTrees() ) { return; }

    static constexpr slice kEncryptable
            = R"({"foo":1234,"nested":[0,1,{"SSN":{"@type":"encryptable","value":"123-45-6789"}},3,4]})",
            kNotEncryptable = R"({"foo":1234,"nested":[0,1,{"SSN":{"type":"encryptable","value":"123-45-6789"}},3,4]})";

    slice json;
    SECTION("verify sha1 digest") { json = kNotEncryptable; }
    SECTION("verify encryptable") { json = kEncryptable; }
    if ( !json ) { return; }
    bool clear = json != kEncryptable;

    fleece::alloc_slice fleeceBody;
    {
        TransactionHelper t(db);
        SharedEncoder     enc(c4db_getSharedFleeceEncoder(db));
        enc.convertJSON(json);
        fleeceBody = enc.finish();
    }
    std::string rid = C4Test::createFleeceRev(db, C4STR("doc"), nullslice, json, 0);
    // rid == "1-<digest>"
    rid              = rid.substr(2);
    alloc_slice sha1 = digest(fleeceBody, nullslice, false);
    if ( clear ) {
        REQUIRE(sha1.asString() == rid);
    } else {
        CHECK(sha1.asString() != rid.substr(2));
        CHECK(sha1.asString().length() == rid.length());
    }
}
