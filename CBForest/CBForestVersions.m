//
//  CBForestVersions.h
//  CBForest
//
//  Created by Jens Alfke on 12/3/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestVersions.h"
#import "CBForestPrivate.h"
#import "rev_tree.h"


@implementation CBForestVersions
{
    CBForestDocument* _doc;
    uint64_t _bp;
    RevTree* _tree;
    BOOL _changed;
    NSMutableArray* _insertedData;
}


- (id) initWithDocument: (CBForestDocument*)doc
                  error: (NSError**)outError
{
    NSParameterAssert(doc);
    self = [super init];
    if (self) {
        _doc = doc;
        if (_doc.exists) {
            if ((_doc.info->content_meta & COUCH_DOC_IS_REVISIONED) != 0) {
                NSData* data = [_doc readData: outError];
                if (!data)
                    return nil;
                _tree = RevTreeDecode(DataToBuf(data), 1);
            }
            if (!_tree) {
                if (outError)
                    *outError = [NSError errorWithDomain: CBForestErrorDomain
                                                    code: COUCHSTORE_ERROR_CORRUPT
                                                userInfo: nil];
                return nil;
            }
            _bp = _doc.info->bp;
        } else {
            _tree = RevTreeNew(1);
        }
    }
    return self;
}


- (void)dealloc {
    RevTreeFree(_tree);
}


- (fdb_handle*) db {
    return _doc.store.db;
}


- (BOOL) save: (NSError**)outError {
    if (!_changed)
        return YES;

    uint32_t generation;
    sized_buf digest;
    const RevNode* curNode = RevTreeGetNode(_tree, 0);
    if (!curNode || !ParseRevID(curNode->revID, &generation, &digest)) {
        generation = 0;
        digest.size = 0;
    }
    _doc.revSequence = generation;
    _doc.revMeta = BufToData(digest);
    _doc.info->content_meta = COUCH_DOC_IS_REVISIONED | COUCH_DOC_IS_COMPRESSED |
                              COUCH_DOC_INVALID_JSON;

    sized_buf encoded = RevTreeEncode(_tree);
    BOOL ok = [_doc writeBytes: encoded.buf length: encoded.size error: outError];
    free(encoded.buf);
    if (!ok)
        return NO;
    _bp = _doc.info->bp;
    _changed = NO;
    _insertedData = nil;
    return YES;
}


- (NSUInteger) revisionCount {
    return RevTreeGetCount(_tree);
}


- (NSString*) currentRevisionID {
    RevTreeSort(_tree);
    const RevNode* current = RevTreeGetNode(_tree, 0);
    return current ? BufToString(current->revID) : nil;
}

- (BOOL) currentRevisionDeleted {
    RevTreeSort(_tree);
    const RevNode* current = RevTreeGetNode(_tree, 0);
    return current && (current->flags & kRevNodeIsDeleted);
}

static NSData* dataForNode(fdb_handle* db, const RevNode* node, NSError** outError) {
    if (outError)
        *outError = nil;
    if (!node)
        return nil;
    sized_buf databuf;
    bool freeData;
    if (!Check(RevTreeReadNodeData(node, db, &databuf, &freeData), outError))
        return nil;
    return BufToData(databuf);
}

- (NSData*) currentRevisionData {
    RevTreeSort(_tree);
    return dataForNode(self.db, RevTreeGetNode(_tree, 0), NULL);
}

- (NSData*) dataOfRevision: (NSString*)revID {
    return dataForNode(self.db, RevTreeFindNode(_tree, StringToBuf(revID)), NULL);
}

- (BOOL) isRevisionDeleted: (NSString*)revID {
    const RevNode* node = RevTreeFindNode(_tree, StringToBuf(revID));
    return node && (node->flags & kRevNodeIsDeleted);
}

- (BOOL) hasRevision: (NSString*)revID {
    return RevTreeFindNode(_tree, StringToBuf(revID)) != NULL;
}

static BOOL nodeIsActive(const RevNode* node) {
    return node && (node->flags & kRevNodeIsLeaf) && !(node->flags & kRevNodeIsDeleted);
}

- (BOOL) hasConflicts {
    // Active (undeleted leaf) nodes come first in the array, so check the second one:
    RevTreeSort(_tree);
    return nodeIsActive(RevTreeGetNode(_tree, 1));
}

- (NSArray*) currentRevisionIDs {
    RevTreeSort(_tree);
    NSMutableArray* conflicts = [NSMutableArray array];
    for (unsigned i = 0; YES; ++i) {
        const RevNode* node = RevTreeGetNode(_tree, i);
        if (!nodeIsActive(node))
            break;
        [conflicts addObject: BufToString(node->revID)];
    }
    return conflicts;
}

- (NSArray*) historyOfRevision: (NSString*)revID {
    RevTreeSort(_tree);
    const RevNode* node;
    if (revID)
        node = RevTreeFindNode(_tree, StringToBuf(revID));
    else
        node = RevTreeGetNode(_tree, 0);
    
    NSMutableArray* history = [NSMutableArray array];
    while (node) {
        [history addObject: BufToString(node->revID)];
        node = RevTreeGetNode(_tree, node->parentIndex);
    }
    return history;
}

- (BOOL) addRevision: (NSData*)data withID: (NSString*)revID parent: (NSString*)parentRevID {
    if (!RevTreeReserveCapacity(&_tree, 1))
        return NO;

    // Make sure the given revID is valid but doesn't exist yet:
    NSData* revIDData = [revID dataUsingEncoding: NSUTF8StringEncoding];
    sized_buf revIDBuf = DataToBuf(revIDData);
    uint32_t newSeq;
    if (!ParseRevID(revIDBuf, &newSeq, NULL))
        return NO;
    if (RevTreeFindNode(_tree, revIDBuf))
        return NO;

    // Find the parent node, if a parent ID is given:
    const RevNode* parent = NULL;
    uint32_t parentSeq = 0;
    if (parentRevID) {
        sized_buf parentIDBuf = StringToBuf(parentRevID);
        parent = RevTreeFindNode(_tree, parentIDBuf);
        if (!parent || !ParseRevID(parentIDBuf, &parentSeq, NULL))
            return NO;
    }
    
    // Enforce that sequence number went up by 1 from the parent:
    if (newSeq != parentSeq + 1)
        return NO;

    // Keep references to the NSData objects that the sized_bufs point to, so their contents
    // aren't destroyed, because RevTreeInsert is going to keep pointers to them:
    if (!_insertedData)
        _insertedData = [NSMutableArray array];
    [_insertedData addObject: revIDData];
    if (data) {
        data = [data copy];
        [_insertedData addObject: data];
    }

    // Finally, insert:
    RevTreeInsert(_tree, revIDBuf, DataToBuf(data), parent, (data == nil), _bp);
    _changed = YES;
    return YES;
}

@end
