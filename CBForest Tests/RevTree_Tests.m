//
//  RevTree_Tests.m
//  CBForest
//
//  Created by Jens Alfke on 3/28/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "rev_tree.h"
#import "varint.h"


static sized_buf strtobuf(const char* str) {return (sized_buf){(void*)str, strlen(str)};}
static bool bufequalstr(sized_buf buf, const char* str) {
    return buf.size == strlen(str) && memcmp(buf.buf, str, buf.size) == 0;
}


@interface RevTree_Tests : XCTestCase
@end


@implementation RevTree_Tests
{
    RevTree* tree;
}

- (void) setUp {
    [super setUp];
}

- (void) tearDown {
    RevTreeFree(tree);
    tree = nil;
    [super tearDown];
}

- (void) testVarint {
    const uint64_t tests[] = {0, 1, 127, 128, 123456, 0x12345678, 0x1234567812345678,
        0x7FFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF};
    uint8_t buf[kMaxVarintLen64];
    for (int i=0; i<9; i++) {
        size_t len = PutUVarInt(buf, tests[i]);
        XCTAssert(len > 0 && len <= kMaxVarintLen64);
        XCTAssertEqual(len, SizeOfVarInt(tests[i]));
        uint64_t readNum;
        size_t readLen = GetUVarInt((sized_buf){buf, len}, &readNum);
        XCTAssertEqual(readLen, len);
        XCTAssertEqual(readNum, tests[i]);
    }
}

- (void) testParseRevID {
    unsigned sequence;
    sized_buf digest;
    XCTAssert(RevIDParse(strtobuf("42-cafebabe"), &sequence, &digest));
    XCTAssertEqual(sequence, 42);
    XCTAssert(bufequalstr(digest, "cafebabe"));

    XCTAssert(!RevIDParse(strtobuf(""), &sequence, &digest));
    XCTAssert(!RevIDParse(strtobuf("0-cafebabe"), &sequence, &digest));
    XCTAssert(!RevIDParse(strtobuf("-cafebabe"), &sequence, &digest));
    XCTAssert(!RevIDParse(strtobuf("10-"), &sequence, &digest));
    XCTAssert(!RevIDParse(strtobuf("111111111111111-foo"), &sequence, &digest));
    XCTAssert(!RevIDParse(strtobuf("1af9-decafbad"), &sequence, &digest));
}

- (void) testNewTree {
    tree = RevTreeNew(1);
    XCTAssert(tree != nil);
    XCTAssertEqual(RevTreeGetCount(tree), 0);
    XCTAssertEqual(RevTreeGetCurrentNode(tree), NULL);
    XCTAssertEqual(RevTreeGetNode(tree, 0), NULL);
    XCTAssertEqual(RevTreeFindNode(tree, strtobuf("1-deadbeef")), NULL);
    XCTAssert(!RevTreeHasConflict(tree));
}

- (void) testInsertRev {
    // Create a tree and insert a revision:
    tree = RevTreeNew(1);
    RevTreeInsert(&tree, strtobuf("1-f00"), strtobuf("{\"hi\":true}"), false, (sized_buf){}, false);
    XCTAssertEqual(RevTreeGetCount(tree), 1);
    const RevNode* node = RevTreeGetNode(tree, 0);
    XCTAssert(node != nil);
    XCTAssert(bufequalstr(node->revID, "1-f00"));
    XCTAssert(bufequalstr(node->data, "{\"hi\":true}"));
    XCTAssertEqual(node->parentIndex, kRevNodeParentIndexNone);
    XCTAssertEqual(node->flags, kRevNodeIsLeaf | kRevNodeIsNew);

    XCTAssertEqual(RevTreeGetNode(tree, 1), NULL);
    XCTAssertEqual(RevTreeGetCurrentNode(tree), node);
    XCTAssertEqual(RevTreeFindNode(tree, strtobuf("1-f00")), node);
    XCTAssertEqual(RevTreeFindNode(tree, strtobuf("1-deadbeef")), NULL);
    XCTAssert(!RevTreeHasConflict(tree));

    // Reserve capacity. This will realloc the tree:
    RevTreeReserveCapacity(&tree, 1);
    XCTAssertEqual(RevTreeGetCount(tree), 1);
    node = RevTreeGetNode(tree, 0);
    XCTAssert(node != nil);
    XCTAssert(bufequalstr(node->revID, "1-f00"));

    // Insert a new revision:
    RevTreeInsert(&tree, strtobuf("2-ba4"), strtobuf("{\"hi\":false}"), false, node->revID, false);
    XCTAssertEqual(RevTreeGetCount(tree), 2);
    const RevNode* node2 = RevTreeGetNode(tree, 1);
    XCTAssert(node2 != nil);
    XCTAssert(bufequalstr(node2->revID, "2-ba4"));
    XCTAssert(bufequalstr(node2->data, "{\"hi\":false}"));
    XCTAssertEqual(node2->parentIndex, 0);
    XCTAssertEqual(node2->flags, kRevNodeIsLeaf | kRevNodeIsNew);
    XCTAssertEqual(node->flags, kRevNodeIsNew);

    // Sort the revisions: this will change their order:
    RevTreeSort(tree);
    node = RevTreeFindNode(tree, strtobuf("1-f00"));
    XCTAssertEqual(node, RevTreeGetNode(tree, 1));
    XCTAssertEqual(node->parentIndex, kRevNodeParentIndexNone);
    node2 = RevTreeFindNode(tree, strtobuf("2-ba4"));
    XCTAssertEqual(node2, RevTreeGetNode(tree, 0));
    XCTAssertEqual(node2->parentIndex, 1);
}

- (void) testEncode {
    tree = RevTreeNew(1);
    sized_buf revID1 = strtobuf("1-f000");
    RevTreeInsert(&tree, revID1, strtobuf("{\"hi\":true}"), false, (sized_buf){}, false);
    sized_buf encoded = RevTreeEncode(tree);
    XCTAssert(encoded.buf != NULL);
    XCTAssert(encoded.size > 20);

    tree = RevTreeDecode(encoded, 1, 88, 123);
    XCTAssert(tree != NULL);
    XCTAssertEqual(RevTreeGetCount(tree), 1);
    const RevNode* rev1 = RevTreeFindNode(tree, revID1);
    XCTAssert(rev1->data.buf != NULL);
    sized_buf revID2 = strtobuf("2-ba22");
    RevTreeInsert(&tree, revID2, strtobuf("{\"hi\":false}"), false, revID1, false);

    sized_buf encoded2 = RevTreeEncode(tree);
    XCTAssert(encoded2.buf != NULL);
    XCTAssert(encoded2.size > 20);

    tree = RevTreeDecode(encoded2, 1, 1, 456);
    XCTAssert(tree != NULL);
    XCTAssertEqual(RevTreeGetCount(tree), 2);
    rev1 = RevTreeFindNode(tree, revID1);
#ifdef REVTREE_USES_FILE_OFFSETS
    XCTAssert(rev1->data.buf == NULL);
    XCTAssertEqual(rev1->oldBodyOffset, 123);
    XCTAssertEqual(rev1->sequence, 88);
#endif
    const RevNode* rev2 = RevTreeFindNode(tree, revID2);
    XCTAssert(rev2->data.buf != NULL);
}

@end
