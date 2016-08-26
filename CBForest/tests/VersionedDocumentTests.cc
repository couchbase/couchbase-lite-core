//
//  VersionedDocument_Tests.mm
//  CBForest
//
//  Created by Jens Alfke on 5/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "VersionedDocument.hh"
#include "CBForestTest.hh"


static revidBuffer stringToRev(string str) {
    revidBuffer buf(str);
    return buf;
}


TEST_CASE("VersionedDocument RevIDs", "[VersionedDocument]") {
    revidBuffer rev(slice("1-f0f0"));
    REQUIRE((string)rev == string("1-f0f0"));
    static const uint8_t expectedBytes[] = {0x01, 0xf0, 0xf0};
    REQUIRE(rev == slice(expectedBytes, sizeof(expectedBytes)));

    rev = stringToRev("1234-1234567890abcdef1234567890abcdef");
    REQUIRE((string)rev == string("1234-1234567890abcdef1234567890abcdef"));
    static const uint8_t expectedBytes2[18] = {0xd2, 0x09, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD,
        0xEF, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF};
    REQUIRE(rev == slice(expectedBytes2, sizeof(expectedBytes2)));

    // New-style ('clock') revID:
    rev.parseNew(slice("17@snej"));
    REQUIRE(rev.isClock());
    REQUIRE(rev.generation() == 17u);
    REQUIRE(rev.digest() == slice("snej"));
    static const uint8_t expectedBytes3[] = {0x00, 0x11, 's', 'n', 'e', 'j'};
    REQUIRE(rev == slice(expectedBytes3, sizeof(expectedBytes3)));
}


TEST_CASE("VersionedDocument BadRevIDs", "[VersionedDocument]") {
    // Check a bunch of invalid revIDs to make sure they all correctly fail to parse:
    static const char* kBadStrings[] = {
        "",
        "",
        "1",
        "@snej",
        "snej@x",
        "0@snej",
        "12345678901234567890123@snej",
        "1234@abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz",
        "-",
        "1-",
        "-ff",
        "1-snej",
        "1-abc",
        "0-cafe",
        "1-123",
        "12345678901234567890123-cafe",
        "123-f@bb",
    };
    for (int i = 0; i < sizeof(kBadStrings)/sizeof(char*); i++) {
        revidBuffer rev;
        REQUIRE_FALSE(rev.tryParse(slice(kBadStrings[i]), true));
    }

    // Make sure we don't parse new-style IDs with the old parser:
    revidBuffer rev;
    REQUIRE_FALSE(rev.tryParse(slice("17@snej"), false));
}


TEST_CASE_METHOD (DataFileTestFixture, "VersionedDocument Empty", "[VersionedDocument]") {
    VersionedDocument v(*store, slice("foo"));
    REQUIRE(v.docID() == slice("foo"));
    REQUIRE(v.revID() == revid());
    REQUIRE(v.flags() == (VersionedDocument::Flags)0);
    REQUIRE(v.get(stringToRev("1-aaaa")) == NULL);
}


TEST_CASE_METHOD (DataFileTestFixture, "VersionedDocument RevTreeInsert", "[VersionedDocument]") {
    RevTree tree;
    const Rev* rev;
    revidBuffer rev1ID(cbforest::slice("1-aaaa"));
    cbforest::slice rev1Data("body of revision");
    int httpStatus;
    rev = tree.insert(rev1ID, rev1Data, false, false,
                      revid(), false, httpStatus);
    REQUIRE(rev);
    REQUIRE(httpStatus == 201);
    REQUIRE(rev->revID == rev1ID);
    REQUIRE(rev->inlineBody() == rev1Data);
    REQUIRE(rev->parent() == (const Rev*)nullptr);
    REQUIRE_FALSE(rev->isDeleted());

    revidBuffer rev2ID(cbforest::slice("2-bbbb"));
    cbforest::slice rev2Data("second revision");
    auto rev2 = tree.insert(rev2ID, rev2Data, false, false, rev1ID, false, httpStatus);
    REQUIRE(rev2);
    REQUIRE(httpStatus == 201);
    REQUIRE(rev2->revID == rev2ID);
    REQUIRE(rev2->inlineBody() == rev2Data);
    REQUIRE_FALSE(rev2->isDeleted());

    tree.sort();
    rev = tree.get(rev1ID);
    rev2 = tree.get(rev2ID);
    REQUIRE(rev);
    REQUIRE(rev2);
    REQUIRE(rev2->parent() == rev);
    REQUIRE(rev->parent() == NULL);

    REQUIRE(tree.currentRevision() == rev2);
    REQUIRE_FALSE(tree.hasConflict());

    tree.sort();
    REQUIRE(tree[0] == rev2);
    REQUIRE(tree[1] == rev);
    REQUIRE(rev->index() == 1u);
    REQUIRE(rev2->index() == 0u);

    alloc_slice ext = tree.encode();

    RevTree tree2(ext, 12, 1234);
}


TEST_CASE_METHOD (DataFileTestFixture, "VersionedDocument AddRevision", "[VersionedDocument]") {
    string revID = "1-fadebead", body = "{\"hello\":true}";
    revidBuffer revIDBuf(revID);
    VersionedDocument v(*store, slice("foo"));
    int httpStatus;
    v.insert(revIDBuf, body, false, false, NULL, false, httpStatus);
    REQUIRE(httpStatus == 201);

    const Rev* node = v.get(stringToRev(revID));
    REQUIRE(node);
    REQUIRE_FALSE(node->isDeleted());
    REQUIRE(node->isLeaf());
    REQUIRE(node->isActive());
    REQUIRE(v.size() == 1);
    REQUIRE(v.currentRevisions().size() == 1);
    REQUIRE(v.currentRevisions()[0] == v.currentRevision());
}


TEST_CASE_METHOD (DataFileTestFixture, "VersionedDocument DocType", "[VersionedDocument]") {
    revidBuffer rev1ID(cbforest::slice("1-aaaa"));
    {
        VersionedDocument v(*store, slice("foo"));

        cbforest::slice rev1Data("body of revision");
        int httpStatus;
        v.insert(rev1ID, rev1Data, true /*deleted*/, false,
                 revid(), false, httpStatus);

        v.setDocType(slice("moose"));
        REQUIRE(v.docType() == slice("moose"));
        Transaction t(db);
        v.save(t);
    }
    {
        VersionedDocument v(*store, slice("foo"));
        REQUIRE((int)v.flags() == (int)VersionedDocument::kDeleted);
        REQUIRE(v.revID() == (revid)rev1ID);
        REQUIRE(v.docType() == slice("moose"));
    }
}
