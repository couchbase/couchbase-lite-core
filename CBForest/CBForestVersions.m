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
#import "forestdb_x.h"


#define kDefaultMaxDepth 100


@implementation CBForestVersions
{
    CBForestDocument* _doc;
    uint64_t _bp;
    NSData* _rawTree;
    RevTree* _tree;
    BOOL _changed;
    NSMutableArray* _insertedData;
}


@synthesize maxDepth=_maxDepth;


- (id) initWithDocument: (CBForestDocument*)doc
                  error: (NSError**)outError
{
    NSParameterAssert(doc);
    self = [super init];
    if (self) {
        _maxDepth = kDefaultMaxDepth;
        _doc = doc;
        if (_doc.exists) {
            // Read RevTree from doc body:
            _rawTree = [_doc getBody: outError];
            if (!_rawTree)
                return nil;
            NSAssert(doc.bodyFileOffset > 0, @"Body offset unknown");
            _tree = RevTreeDecode(DataToBuf(_rawTree), 1,
                                  doc.sequence, doc.bodyFileOffset);
            if (!_tree) {
                if (outError)
                    *outError = [NSError errorWithDomain: CBForestErrorDomain
                                                    code: kCBForestErrorDataCorrupt
                                                userInfo: nil];
                return nil;
            }
            _bp = _doc.bodyFileOffset;
        } else {
            // New doc, create a new RevTree:
            _tree = RevTreeNew(1);
        }
    }
    return self;
}


- (void)dealloc {
    RevTreeFree(_tree);
}


- (fdb_handle*) db {
    return _doc.db.db;
}


- (BOOL) save: (NSError**)outError {
    if (!_changed)
        return YES;

    RevTreePrune(_tree, _maxDepth);
    sized_buf encoded = RevTreeEncode(_tree);
    [_doc setBodyBytes: encoded.buf length: encoded.size noCopy: YES];

    if (![_doc saveChanges: outError])
        return NO;
    _bp = _doc.bodyFileOffset;
    [_doc unloadBody];
    _changed = NO;
    return YES;
}


- (NSUInteger) revisionCount {
    return RevTreeGetCount(_tree);
}


static NSData* dataForNode(fdb_handle* db, const RevNode* node, NSError** outError) {
    if (outError)
        *outError = nil;
    if (!node)
        return nil;
    NSData* result = nil;
    if (node->data.size > 0) {
        result = BufToData(node->data.buf, node->data.size);
    }
#ifdef REVTREE_USES_FILE_OFFSETS
    else if (node->oldBodyOffset > 0) {
        // Look up old document from the saved oldBodyOffset:
        fdb_doc doc = {.bodylen = node->oldBodySize};
        if (!Check(x_fdb_read_body(db, &doc, node->oldBodyOffset), outError))
            return nil;
        RevTree* oldTree = RevTreeDecode((sized_buf){doc.body, doc.bodylen}, 0, 0, 0);
        if (oldTree) {
            // Now look up the revision, which still has a body in this old doc:
            const RevNode* oldNode = RevTreeFindNode(oldTree, node->revID);
            if (oldNode && oldNode->data.buf)
                result = BufToData(oldNode->data.buf, oldNode->data.size);
            RevTreeFree(oldTree);
        }
        free(doc.body);
        if (!result && outError)
            *outError = [NSError errorWithDomain: CBForestErrorDomain
                                            code: kCBForestErrorDataCorrupt
                                        userInfo: nil];
    }
#endif
    return result;
}

- (NSData*) currentRevisionData {
    RevTreeSort(_tree);
    return dataForNode(self.db, RevTreeGetNode(_tree, 0), NULL);
}

- (NSData*) dataOfRevision: (NSString*)revID {
    return dataForNode(self.db, RevTreeFindNode(_tree, CompactRevIDToBuf(revID)), NULL);
}

- (BOOL) isRevisionDeleted: (NSString*)revID {
    const RevNode* node = RevTreeFindNode(_tree, CompactRevIDToBuf(revID));
    return node && (node->flags & kRevNodeIsDeleted);
}

- (BOOL) hasRevision: (NSString*)revID {
    return RevTreeFindNode(_tree, CompactRevIDToBuf(revID)) != NULL;
}

static BOOL nodeIsActive(const RevNode* node) {
    return node && (node->flags & kRevNodeIsLeaf) && !(node->flags & kRevNodeIsDeleted);
}

- (BOOL) hasConflicts {
    return RevTreeHasConflict(_tree);
}

- (NSArray*) currentRevisionIDs {
    RevTreeSort(_tree);
    NSMutableArray* conflicts = [NSMutableArray array];
    for (unsigned i = 0; YES; ++i) {
        const RevNode* node = RevTreeGetNode(_tree, i);
        if (!nodeIsActive(node))
            break;
        [conflicts addObject: ExpandRevID(node->revID)];
    }
    return conflicts;
}

- (NSArray*) historyOfRevision: (NSString*)revID {
    RevTreeSort(_tree);
    const RevNode* node;
    if (revID)
        node = RevTreeFindNode(_tree, CompactRevIDToBuf(revID));
    else
        node = RevTreeGetNode(_tree, 0);
    
    NSMutableArray* history = [NSMutableArray array];
    while (node) {
        [history addObject: ExpandRevID(node->revID)];
        node = RevTreeGetNode(_tree, node->parentIndex);
    }
    return history;
}


- (BOOL) addRevision: (NSData*)data
            deletion: (BOOL)deletion
              withID: (NSString*)revID
            parentID: (NSString*)parentRevID
{
    NSData* revIDData = CompactRevID(revID);
    data = [data copy];

    if (!RevTreeInsert(&_tree, DataToBuf(revIDData), DataToBuf(data),
                       CompactRevIDToBuf(parentRevID), deletion, _bp))
        return NO;

    // Keep references to the NSData objects that the sized_bufs point to, so their contents
    // aren't destroyed, because _tree now contains pointers to them:
    if (!_insertedData)
        _insertedData = [NSMutableArray array];
    [_insertedData addObject: revIDData];
    if (data)
        [_insertedData addObject: data];

    // Update the flags and current revision ID.
    // (Remember that the newly-inserted revision did not necessarily become the current one!)
    const RevNode* curNode = RevTreeGetCurrentNode(_tree);
    CBForestDocumentFlags flags = 0;
    if (curNode->flags & kRevNodeIsDeleted)
        flags |= kCBForestDocDeleted;
    if (RevTreeHasConflict(_tree))
        flags |= kCBForestDocConflicted;
    _doc.flags = flags;
    _doc.revID = ExpandRevID(curNode->revID);

    _changed = YES;
    return YES;
}

@end
