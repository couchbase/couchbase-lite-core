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
#import "option.h"


#define kDefaultMaxDepth 100


@interface CBForestVersions ()
@property (readwrite) CBForestVersionsFlags flags;
@property (readwrite) NSString* revID;
@end


@implementation CBForestVersions
{
    CBForestDocument* _doc;
    NSData* _rawTree;
    RevTree* _tree;
    BOOL _changed;
    NSMutableArray* _insertedData;
}


@synthesize maxDepth=_maxDepth, flags=_flags, revID=_revID;


- (id) initWithDB: (CBForestDB*)store docID: (NSString*)docID {
    self = [super initWithDB: store docID: docID];
    if (self) {
        _maxDepth = kDefaultMaxDepth;
        _tree = RevTreeNew(1);
    }
    return self;
}

- (id) initWithDB: (CBForestDB*)store
                info: (const fdb_doc*)info
              offset: (uint64_t)bodyOffset
{
    self = [super initWithDB: store info: info offset: bodyOffset];
    if (self) {
        _maxDepth = kDefaultMaxDepth;
    }
    return self;
}


- (BOOL) readTree: (NSError**)outError {
    _rawTree = [self readBody: outError];
    if (!_rawTree)
        return NO;
    NSAssert(self.bodyFileOffset > 0, @"Body offset unknown");
    RevTree* tree = RevTreeDecode(DataToBuf(_rawTree), 1, self.sequence, self.bodyFileOffset);
    if (!tree) {
        if (outError)
            *outError = [NSError errorWithDomain: CBForestErrorDomain
                                            code: kCBForestErrorDataCorrupt
                                        userInfo: nil];
        return NO;
    }
    RevTreeFree(_tree);
    _tree = tree;
    return YES;
}


- (void) dealloc {
    RevTreeFree(_tree);
}


static CBForestVersionsFlags flagsFromMeta(const fdb_doc* docinfo) {
    if (docinfo->metalen == 0)
        return 0;
    return ((UInt8*)docinfo->meta)[0];
}


- (BOOL) reloadMeta:(NSError *__autoreleasing *)outError {
    if (![super reloadMeta: outError])
        return NO;
    // Decode flags & revID from metadata:
    NSData* meta = self.metadata;
    if (meta.length >= 1) {
        const void* metabytes = meta.bytes;
        CBForestVersionsFlags flags = *(uint8_t*)metabytes;
        NSString* revID = ExpandRevID((sized_buf){(void*)metabytes+1, meta.length-1});
        if (flags != _flags || ![revID isEqualToString: _revID]) {
            self.flags = flags;
            self.revID = revID;
            if (![self readTree: outError])
                return NO;
        }
    } else {
        self.flags = 0;
        self.revID = nil;
    }
    return YES;
}


- (BOOL) save: (NSError**)outError {
    if (!_changed)
        return YES;

    RevTreePrune(_tree, _maxDepth);
    sized_buf encoded = RevTreeEncode(_tree);

    // Encode flags & revID into metadata:
    NSMutableData* metadata = [NSMutableData dataWithLength: 1 + _revID.length];
    void* bytes = metadata.mutableBytes;
    *(uint8_t*)bytes = _flags;
    sized_buf dstRev = {bytes+1, metadata.length-1};
    RevIDCompact(StringToBuf(_revID), &dstRev);
    metadata.length = 1 + dstRev.size;

    BOOL ok = [self writeBody: BufToData(encoded.buf, encoded.size)
                     metadata: metadata
                        error: outError];
    free(encoded.buf);
    if (ok)
        _changed = NO;
    return ok;
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
        fdb_doc doc = {.seqnum = node->sequence};
        if (!Check(fdb_get_byoffset(db, &doc, node->oldBodyOffset), outError))
            return nil; // This will happen if the old doc body was lost by compaction.
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
    return dataForNode(self.db.handle, RevTreeGetNode(_tree, 0), NULL);
}

- (NSData*) dataOfRevision: (NSString*)revID {
    return dataForNode(self.db.handle, RevTreeFindNode(_tree, CompactRevIDToBuf(revID)), NULL);
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
                       CompactRevIDToBuf(parentRevID), deletion))
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
    CBForestVersionsFlags flags = 0;
    if (curNode->flags & kRevNodeIsDeleted)
        flags |= kCBForestDocDeleted;
    if (RevTreeHasConflict(_tree))
        flags |= kCBForestDocConflicted;
    self.flags = flags;
    self.revID = ExpandRevID(curNode->revID);

    _changed = YES;
    return YES;
}


+ (BOOL) docInfo: (const fdb_doc*)docInfo
  matchesOptions: (const CBForestEnumerationOptions*)options
{
    if (!options || !options->includeDeleted) {
        if (flagsFromMeta(docInfo) & kCBForestDocDeleted)
            return NO;
    }
    if (options && options->onlyConflicts) {
        if (!(flagsFromMeta(docInfo) & kCBForestDocConflicted))
            return NO;
    }
    return YES;
}


@end
