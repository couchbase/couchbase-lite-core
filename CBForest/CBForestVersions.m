//
//  CBForestVersions.m
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


@implementation CBForestVersions
{
    CBForestDocument* _doc;
    NSData* _rawTree;
    RevTree* _tree;
    BOOL _changed;
    NSMutableArray* _insertedData;
    CBForestVersionsFlags _flags;
    NSString* _revID;
}


@synthesize maxDepth=_maxDepth, flags=_flags;


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
          options: (CBForestContentOptions)options
            error:(NSError**)outError
{
    self = [super initWithDB: store info: info offset: bodyOffset options: options error: outError];
    if (self) {
        _maxDepth = kDefaultMaxDepth;
        [self readFlags];
        if (!(options & kCBForestDBMetaOnly)) {
            if (![self readTree: outError])
                return nil;
        }
    }
    return self;
}


- (BOOL) readTree: (NSError**)outError {
    _rawTree = [super readBody: outError];
    if (!_rawTree)
        return NO;
    NSAssert(self.bodyFileOffset > 0, @"Body offset unknown");
    RevTree* tree = RevTreeDecode(DataToSlice(_rawTree), 1, self.sequence, self.bodyFileOffset);
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


- (NSData*) readBody: (NSError**)outError {
    return _rawTree ?: [super readBody: outError];
}


- (void) dealloc {
    RevTreeFree(_tree);
}


static CBForestVersionsFlags flagsFromMeta(const fdb_doc* docinfo) {
    if (docinfo->metalen == 0)
        return 0;
    return ((UInt8*)docinfo->meta)[0];
}


- (void) readFlags {
    NSData* meta = self.metadata;
    if (meta.length < 1)
        _flags = 0;
    else
        _flags = *(uint8_t*)meta.bytes;
}


- (NSString*) revID {
    if (!_revID) {
        NSData* meta = self.metadata;
        if (meta.length > 1) {
            const void* metabytes = meta.bytes;
            _revID = ExpandRevID((slice){(void*)metabytes+1, meta.length-1});
        }
    }
    return _revID;
}


- (BOOL) reloadMeta:(NSError **)outError {
    CBForestSequence oldSequence = self.sequence;
    if (![super reloadMeta: outError])
        return NO;
    [self readFlags];
    _revID = nil;
    if (self.sequence != oldSequence) {
        RevTreeFree(_tree);
        _tree = NULL;
    }
    return YES;
}


- (BOOL) reload: (CBForestContentOptions)options error: (NSError **)outError {
    if (![self reloadMeta: outError])
        return NO;
    else if ((options & kCBForestDBMetaOnly) || _tree)
        return YES;
    else if (self.bodyFileOffset > 0)
        return [self readTree: outError];
    else if (options & kCBForestDBCreate) {
        _tree = RevTreeNew(1);
        return YES;
    } else
        return Check(FDB_RESULT_KEY_NOT_FOUND, outError);
}


- (BOOL) save: (NSError**)outError {
    if (!_changed)
        return YES;

    RevTreePrune(_tree, _maxDepth);
    slice encoded = RevTreeEncode(_tree);

    // Encode flags & revID into metadata:
    NSMutableData* metadata = [NSMutableData dataWithLength: 1 + _revID.length];
    void* bytes = metadata.mutableBytes;
    *(uint8_t*)bytes = _flags;
    slice dstRev = {bytes+1, metadata.length-1};
    RevIDCompact(StringToSlice(_revID), &dstRev);
    metadata.length = 1 + dstRev.size;

    BOOL ok = [self writeBody: SliceToData(encoded.buf, encoded.size)
                     metadata: metadata
                        error: outError];
    free((void*)encoded.buf);
    if (ok)
        _changed = NO;
    return ok;
}


- (NSUInteger) revisionCount {
    return RevTreeGetCount(_tree);
}


// internal method that looks up a RevNode. If revID is nil, returns the current node.
- (const RevNode*) nodeWithID: (NSString*)revID {
    if (revID)
        return RevTreeFindNode(_tree, CompactRevIDToSlice(revID));
    else {
        RevTreeSort(_tree);
        return RevTreeGetNode(_tree, 0);
    }
}


- (NSString*) currentRevisionID {
    const RevNode* current = [self nodeWithID: nil];
    return current ? ExpandRevID(current->revID) : nil;
}


- (NSData*) dataOfRevision: (NSString*)revID {
    return [self dataOfRevision: revID error: NULL];
}

- (NSData*) dataOfRevision: (NSString*)revID error: (NSError**)outError {
    if (outError)
        *outError = nil;
    const RevNode* node = [self nodeWithID: revID];
    if (!node)
        return nil;
    NSData* result = nil;
    if (node->data.size > 0) {
        result = SliceToData(node->data.buf, node->data.size);
    }
#ifdef REVTREE_USES_FILE_OFFSETS
    else if (node->oldBodyOffset > 0) {
        // Look up old document from the saved oldBodyOffset:
        fdb_doc doc = {.seqnum = node->sequence};
        if (!Check([self.db rawGetBody: &doc byOffset: node->oldBodyOffset], outError))
            return nil; // This will happen if the old doc body was lost by compaction.
        RevTree* oldTree = RevTreeDecode((slice){doc.body, doc.bodylen}, 0, 0, 0);
        if (oldTree) {
            // Now look up the revision, which still has a body in this old doc:
            const RevNode* oldNode = RevTreeFindNode(oldTree, node->revID);
            if (oldNode && oldNode->data.buf)
                result = SliceToData(oldNode->data.buf, oldNode->data.size);
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

- (BOOL) isRevisionDeleted: (NSString*)revID {
    const RevNode* node = [self nodeWithID: revID];
    return node && (node->flags & kRevNodeIsDeleted);
}

- (BOOL) hasRevision: (NSString*)revID {
    return [self nodeWithID: revID] != NULL;
}

- (CBForestRevisionFlags) flagsOfRevision: (NSString*)revID {
    const RevNode* node = [self nodeWithID: revID];
    if (!node)
        return 0;
    CBForestRevisionFlags flags = (CBForestRevisionFlags)node->flags | kCBForestRevisionKnown;
    if (node->data.buf != NULL || node->oldBodyOffset != 0)
        flags |= kCBForestRevisionHasBody;
    return flags;
}

- (NSString*) parentIDOfRevision: (NSString*)revID {
    const RevNode* node = [self nodeWithID: revID];
    if (!node || node->parentIndex == kRevNodeParentIndexNone)
        return nil;
    const RevNode* parent = RevTreeGetNode(_tree, node->parentIndex);
    return ExpandRevID(parent->revID);
}

- (BOOL) hasConflicts {
    return RevTreeHasConflict(_tree);
}

- (NSArray*) allRevisionIDs {
    NSMutableArray* revIDs = [[NSMutableArray alloc] initWithCapacity: RevTreeGetCount(_tree)];
    for (unsigned i = 0; i < RevTreeGetCount(_tree); ++i) {
        [revIDs addObject: ExpandRevID(RevTreeGetNode(_tree, i)->revID)];
    }
    return revIDs;
}

- (NSArray*) currentRevisionIDs {
    RevTreeSort(_tree);
    NSMutableArray* current = [NSMutableArray array];
    for (unsigned i = 0; YES; ++i) {
        const RevNode* node = RevTreeGetNode(_tree, i);
        if (!node)
            break;
        if (node->flags & kRevNodeIsLeaf)
            [current addObject: ExpandRevID(node->revID)];
    }
    return current;
}

- (NSArray*) historyOfRevision: (NSString*)revID {
    RevTreeSort(_tree);
    const RevNode* node;
    if (revID)
        node = [self nodeWithID: revID];
    else
        node = RevTreeGetNode(_tree, 0);
    
    NSMutableArray* history = [NSMutableArray array];
    while (node) {
        [history addObject: ExpandRevID(node->revID)];
        node = RevTreeGetNode(_tree, node->parentIndex);
    }
    return history;
}


#pragma mark - INSERTION:


// Use this to get a sized_buf for any data that's going to be added to the RevTree.
// It adds a reference to the NSData so it won't be dealloced and invalidate the sized_buf.
- (slice) rememberData: (NSData*)data {
    if (!data)
        return (slice){NULL, 0};
    data = [data copy]; // in case it's mutable
    if (!_insertedData)
        _insertedData = [NSMutableArray array];
    [_insertedData addObject: data];
    return DataToSlice(data);
}


// Update the flags and current revision ID.
- (void) updateAfterInsert {
    RevTreeSort(_tree);
    const RevNode* curNode = RevTreeGetCurrentNode(_tree);
    CBForestVersionsFlags flags = 0;
    if (curNode->flags & kRevNodeIsDeleted)
        flags |= kCBForestDocDeleted;
    if (RevTreeHasConflict(_tree))
        flags |= kCBForestDocConflicted;
    _flags = flags;
    _revID = ExpandRevID(curNode->revID);
    _changed = YES;
}


- (BOOL) addRevision: (NSData*)data
            deletion: (BOOL)deletion
              withID: (NSString*)revID
            parentID: (NSString*)parentRevID
       allowConflict: (BOOL)allowConflict
{
    if (!RevTreeInsert(&_tree,
                       [self rememberData: CompactRevID(revID)],
                       [self rememberData: data],
                       deletion,
                       CompactRevIDToSlice(parentRevID),
                       allowConflict))
        return NO;
    [self updateAfterInsert];
    return YES;
}


// Given an NSArray of revision ID strings, converts them to a C array of sized_bufs pointing
// to their compact forms.
- (slice*) revStringsToBufs: (NSArray*)revIDs remember: (BOOL)remember {
    NSUInteger numRevs = revIDs.count;
    slice *revBufs = malloc(numRevs * sizeof(slice));
    if (!revBufs)
        return NULL;
    for (NSUInteger i=0; i<numRevs; i++) {
        NSData* revData = CompactRevID(revIDs[i]);
        if (remember)
            [self rememberData: revData];
        revBufs[i] = DataToSlice(revData);
    }
    return revBufs;
}


- (NSInteger) addRevision: (NSData *)data
                 deletion: (BOOL)deletion
                  history: (NSArray*)history // history[0] is new rev's ID
{
    slice *historyBufs = [self revStringsToBufs: history remember: YES];
    if (!historyBufs)
        return -1;
    int numAdded = RevTreeInsertWithHistory(&_tree, historyBufs, (unsigned)history.count,
                                            [self rememberData: data], deletion);
    free(historyBufs);
    if (numAdded > 0)
        [self updateAfterInsert];
    return numAdded;
}


- (NSArray*) purgeRevisions: (NSArray*)revIDs {
    slice *revBufs = [self revStringsToBufs: revIDs remember: NO];
    if (!revBufs)
        return nil;
    int numPurged = RevTreePurge(_tree, revBufs, (unsigned)revIDs.count);
    if (numPurged > 0)
        _changed = YES;
    NSMutableArray* result = [NSMutableArray array];
    for (NSUInteger i=0; i<revIDs.count; i++) {
        if (revBufs[i].size == 0)  // RevTreePurge removed this rev
            [result addObject: revIDs[i]];
    }
    return result;
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


- (NSString*) dump {
    NSMutableString* out = [NSMutableString stringWithFormat: @"Doc %@ / %@\n",
                            self.docID, self.revID];
    RevTreeSort(_tree);
    for (int i=0; i<RevTreeGetCount(_tree); i++) {
        const RevNode* node = RevTreeGetNode(_tree, i);
        [out appendFormat: @"  #%2d: %@", i, ExpandRevID(node->revID)];
        if (node->flags & kRevNodeIsDeleted)
            [out appendString: @" (DEL)"];
        if (node->flags & kRevNodeIsLeaf)
            [out appendString: @" (leaf)"];
        if (node->flags & kRevNodeIsNew)
            [out appendString: @" (new)"];
        if (node->parentIndex != kRevNodeParentIndexNone)
            [out appendFormat: @" ^%d", node->parentIndex];
        [out appendFormat: @"\n"];
    }
    return out;
}


@end
