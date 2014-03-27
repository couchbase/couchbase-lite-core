//
//  CBForest.m
//  CBForest
//
//  Created by Jens Alfke on 9/4/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForest.h"
#import "CBForestPrivate.h"


NSString* const CBForestErrorDomain = @"CBForest";


//FIX: I have no idea what the right values for these are
static /*const*/ fdb_config kDefaultConfig = { // can't be const due to MB-10672
    .chunksize = sizeof(uint64_t),
    .offsetsize = sizeof(uint64_t),
    .buffercache_size = 4 * 1024 * 1024,
    .wal_threshold = 1024,
    .seqtree_opt = FDB_SEQTREE_USE,
    .durability_opt = FDB_DRB_NONE,
};



@implementation CBForest
{
    fdb_handle _db;
    BOOL _open;
}

@synthesize filename=_path;


- (id) initWithFile: (NSString*)filePath
              error: (NSError**)outError
{
    self = [super init];
    if (self) {
        _path = filePath.copy;
        if (!Check(fdb_open(&_db, (char*)filePath.fileSystemRepresentation, &kDefaultConfig), outError))
            return nil;
        _open = YES;
    }
    return self;
}

- (void) dealloc {
    if (_open)
        fdb_close(&_db);
}

- (fdb_handle*) db {
    NSAssert(_open, @"%@ already closed!", self);
    return &_db;
}

- (void) close {
    fdb_close(self.db);
    _open = NO;
}

- (NSString*) description {
    return [NSString stringWithFormat: @"%@[%@]", [self class], _path];
}

- (BOOL) commit: (NSError**)outError {
    return Check(fdb_commit(self.db), outError);
}

- (BOOL) compactToFile: (NSString*)filePath
                 error: (NSError**)outError
{
    if (Check(fdb_compact(self.db, (char*)filePath.fileSystemRepresentation), outError)) {
        _path = filePath.copy;
        return YES;
    } else {
        return NO;
    }
}


#pragma mark - DOCUMENTS:


- (CBForestDocument*) makeDocumentWithID: (NSString*)docID {
    return [[CBForestDocument alloc] initWithStore: self docID: docID];
}


- (CBForestDocument*) documentWithID: (NSString*)docID
                               error: (NSError**)outError
{
    CBForestDocument* doc = [[CBForestDocument alloc] initWithStore: self docID: docID];
    return [doc loadData: outError] ? doc : nil;
}


- (CBForestDocument*) documentWithSequence: (uint64_t)sequence
                                     error: (NSError**)outError
{
    fdb_doc doc = {
        .seqnum = sequence
    };
    if (!Check(fdb_get_byseq(self.db, &doc), outError))
        return nil;
    return [[CBForestDocument alloc] initWithStore: self info: &doc];
}


#pragma mark - ITERATION:


- (BOOL) enumerateDocsFromID: (NSString*)startID
                        toID: (NSString*)endID
                     options: (CBForestEnumerationOptions)options
                       error: (NSError**)outError
                   withBlock: (CBForestIterator)block
{
    sized_buf startKey = StringToBuf(startID);
    sized_buf endKey = StringToBuf(endID);
    fdb_iterator_opt_t fdbOptions = FDB_ITR_NONE;
    if (options & kCBForestEnumerateMetaOnly)
        fdbOptions |= FDB_ITR_METAONLY;
    fdb_iterator iterator;
    fdb_status status = fdb_iterator_init(self.db, &iterator,
                                          startKey.buf, startKey.size,
                                          endKey.buf, endKey.size, fdbOptions);
    if (!Check(status, outError))
        return NO;

    for (;;) {
        fdb_doc *docinfo;
        status = fdb_iterator_next(&iterator, &docinfo);
        if (status != FDB_RESULT_SUCCESS || docinfo == NULL)
            break; // FDB returns FDB_RESULT_FAIL at end of iteration
        CBForestDocument* doc = [[CBForestDocument alloc] initWithStore: self info: docinfo];
        BOOL stop = NO;
        block(doc, &stop);
        if (stop)
            break;
    }
    fdb_iterator_close(&iterator);
    return YES;
}


@end
