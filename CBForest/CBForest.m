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


@implementation CBForest
{
    fdb_handle _db;
    BOOL _open;
}

@synthesize filename=_path;

- (id) initWithFile: (NSString*)filePath
            options: (CBForestOpenOptions)options
              error: (NSError**)outError
{
    self = [super init];
    if (self) {
        _path = filePath.copy;
        fdb_config config;
        if (!Check(fdb_open(&_db, (char*)filePath.fileSystemRepresentation, &config), outError))
            return nil;
        _open = YES;
    }
    return self;
}

- (void) dealloc
{
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
    if (_open)
        return [NSString stringWithFormat: @"%@[%@]", [self class], _path];
    else
        return [NSString stringWithFormat: @"%@[%@ CLOSED]", [self class], _path];
}

#if 0
- (BOOL) getInfo: (CBForestInfo*)info
           error: (NSError**)outError
{
    DbInfo dbInfo;
    if (!Check(couchstore_db_info(self.db, &dbInfo), outError))
        return NO;
    info->lastSequence = dbInfo.last_sequence;
    info->docCount = dbInfo.doc_count;
    info->deletedCount = dbInfo.deleted_count;
    info->spaceUsed = dbInfo.space_used;
    info->headerPosition = dbInfo.header_position;
    return YES;
}
#endif

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
    return [[CBForestDocument alloc] initWithStore: self docID: docID info: NULL];
}


- (CBForestDocument*) documentWithID: (NSString*)docID
                                 error: (NSError**)outError
{
    sized_buf idbuf = StringToBuf(docID);
    fdb_doc doc = {
        .keylen = idbuf.size,
        .key = idbuf.buf
    };
    if (!Check(fdb_get(self.db, &doc), outError))
        return nil;
    return [[CBForestDocument alloc] initWithStore: self docID: nil info: &doc];
}


- (CBForestDocument*) documentWithSequence: (uint64_t)sequence
                                     error: (NSError**)outError
{
    fdb_doc doc = {
        .seqnum = sequence
    };
    if (!Check(fdb_get_byseq(self.db, &doc), outError))
        return nil;
    return [[CBForestDocument alloc] initWithStore: self docID: nil info: &doc];
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
    if (status == FDB_RESULT_SUCCESS) {
        for (;;) {
            fdb_doc *docinfo;
            status = fdb_iterator_next(&iterator, &docinfo);
            if (status != FDB_RESULT_SUCCESS || docinfo == NULL)
                break;
            CBForestDocument* doc = [[CBForestDocument alloc] initWithStore: self docID: nil info: docinfo];
            BOOL stop = NO;
            block(doc, &stop);
            if (stop)
                break;
        }
        fdb_iterator_close(&iterator);
    }
    return Check(status, outError);
}


@end
