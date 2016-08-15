//
//  VersionedDocument_Tests.mm
//  CBForest
//
//  Created by Jens Alfke on 5/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "VersionedDocument.hh"
#include "CBForestTest.hh"


class VersionedDocumentTests : public DatabaseTestFixture {


static revidBuffer stringToRev(string str) {
    revidBuffer buf(str);
    return buf;
}


void testRevIDs () {
    revidBuffer rev(slice("1-f0f0"));
    AssertEqual((string)rev, string("1-f0f0"));
    static const uint8_t expectedBytes[] = {0x01, 0xf0, 0xf0};
    Assert(rev == slice(expectedBytes, sizeof(expectedBytes)));

    rev = stringToRev("1234-1234567890abcdef1234567890abcdef");
    AssertEqual((string)rev, string("1234-1234567890abcdef1234567890abcdef"));
    static const uint8_t expectedBytes2[18] = {0xd2, 0x09, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD,
        0xEF, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF};
    Assert(rev == slice(expectedBytes2, sizeof(expectedBytes2)));

    // New-style ('clock') revID:
    rev.parseNew(slice("17@snej"));
    Assert(rev.isClock());
    AssertEqual(rev.generation(), 17u);
    AssertEqual(rev.digest(), slice("snej"));
    static const uint8_t expectedBytes3[] = {0x00, 0x11, 's', 'n', 'e', 'j'};
    Assert(rev == slice(expectedBytes3, sizeof(expectedBytes3)));
}

void testBadRevIDs() {
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
        Assert(!rev.tryParse(slice(kBadStrings[i]), true));
    }

    // Make sure we don't parse new-style IDs with the old parser:
    revidBuffer rev;
    Assert(!rev.tryParse(slice("17@snej"), false));
}


void testEmpty() {
    VersionedDocument v(*store, slice("foo"));
    AssertEqual(v.docID(), slice("foo"));
    AssertEqual(v.revID(), revid());
    AssertEqual(v.flags(), (VersionedDocument::Flags)0);
    Assert(v.get(stringToRev("1-aaaa")) == NULL);
}


void testRevTreeInsert() {
    RevTree tree;
    const Rev* rev;
    revidBuffer rev1ID(cbforest::slice("1-aaaa"));
    cbforest::slice rev1Data("body of revision");
    int httpStatus;
    rev = tree.insert(rev1ID, rev1Data, false, false,
                      revid(), false, httpStatus);
    Assert(rev);
    AssertEqual(httpStatus, 201);
    Assert(rev->revID == rev1ID);
    Assert(rev->inlineBody() == rev1Data);
    AssertEqual(rev->parent(), (const Rev*)nullptr);
    Assert(!rev->isDeleted());

    revidBuffer rev2ID(cbforest::slice("2-bbbb"));
    cbforest::slice rev2Data("second revision");
    auto rev2 = tree.insert(rev2ID, rev2Data, false, false, rev1ID, false, httpStatus);
    Assert(rev2);
    AssertEqual(httpStatus, 201);
    Assert(rev2->revID == rev2ID);
    Assert(rev2->inlineBody() == rev2Data);
    Assert(!rev2->isDeleted());

    tree.sort();
    rev = tree.get(rev1ID);
    rev2 = tree.get(rev2ID);
    Assert(rev);
    Assert(rev2);
    AssertEqual(rev2->parent(), rev);
    Assert(rev->parent() == NULL);

    AssertEqual(tree.currentRevision(), rev2);
    Assert(!tree.hasConflict());

    tree.sort();
    AssertEqual(tree[0], rev2);
    AssertEqual(tree[1], rev);
    AssertEqual(rev->index(), 1u);
    AssertEqual(rev2->index(), 0u);

    alloc_slice ext = tree.encode();

    RevTree tree2(ext, 12, 1234);
}

void testAddRevision() {
    string revID = "1-fadebead", body = "{\"hello\":true}";
    revidBuffer revIDBuf(revID);
    VersionedDocument v(*store, slice("foo"));
    int httpStatus;
    v.insert(revIDBuf, body, false, false, NULL, false, httpStatus);
    AssertEqual(httpStatus, 201);

    const Rev* node = v.get(stringToRev(revID));
    Assert(node);
    Assert(!node->isDeleted());
    Assert(node->isLeaf());
    Assert(node->isActive());
    AssertEqual(v.size(), 1ul);
    AssertEqual(v.currentRevisions().size(), 1ul);
    AssertEqual(v.currentRevisions()[0], v.currentRevision());
}

void testDocType() {
    revidBuffer rev1ID(cbforest::slice("1-aaaa"));
    {
        VersionedDocument v(*store, slice("foo"));

        cbforest::slice rev1Data("body of revision");
        int httpStatus;
        v.insert(rev1ID, rev1Data, true /*deleted*/, false,
                 revid(), false, httpStatus);

        v.setDocType(slice("moose"));
        AssertEqual(v.docType(), slice("moose"));
        Transaction t(db);
        v.save(t);
    }
    {
        VersionedDocument v(*store, slice("foo"));
        AssertEqual((int)v.flags(), (int)VersionedDocument::kDeleted);
        AssertEqual(v.revID(), (revid)rev1ID);
        AssertEqual(v.docType(), slice("moose"));
    }
}


    CPPUNIT_TEST_SUITE( VersionedDocumentTests );
    CPPUNIT_TEST( testRevIDs );
    CPPUNIT_TEST( testBadRevIDs );
    CPPUNIT_TEST( testEmpty );
    CPPUNIT_TEST( testRevTreeInsert );
    CPPUNIT_TEST( testAddRevision );
    CPPUNIT_TEST( testDocType );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(VersionedDocumentTests);
