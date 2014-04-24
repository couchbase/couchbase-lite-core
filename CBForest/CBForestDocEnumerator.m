//
//  CBForestDocEnumerator.m
//  CBForest
//
//  Created by Jens Alfke on 4/23/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBForestDocEnumerator.h"
#import "CBForest.h"
#import "CBForestPrivate.h"
#import <forestdb.h>


#define PARALLEL
#define PARALLEL_QUEUE_SIZE 4


@implementation CBForestDocEnumerator
{
    CBForestDB* _db;
    CBForestEnumerationOptions _options;
    CBForestDocEnumeratorNextBlock _next;
    CBForestDocEnumeratorFinishBlock _finish;
    Class _documentClass;
#ifdef PARALLEL
    dispatch_queue_t _dispatch;
    CBForestQueue* _rows;
#endif
}


@synthesize stopBeforeKey=_stopBeforeKey, error=_error;


- (instancetype) init {
    return [super init];
}


- (instancetype) initWithDatabase: (CBForestDB*)db
                          options: (const CBForestEnumerationOptions*)options
                        nextBlock: (CBForestDocEnumeratorNextBlock)nextBlock
                      finishBlock: (CBForestDocEnumeratorFinishBlock)finishBlock
{
    self = [super init];
    if (self) {
        _db = db;
        _documentClass = _db.documentClass;
        if (options)
            _options = *options;
        _next = nextBlock;
        _finish = finishBlock;

#ifdef PARALLEL
        _rows = [[CBForestQueue alloc] initWithCapacity: PARALLEL_QUEUE_SIZE];
        _dispatch = dispatch_queue_create("CBForestDocEnumerator", NULL);
        dispatch_async(_dispatch, ^{
            [self generateRows];
        });
#endif
    }
    return self;
}


- (void) dealloc {
    if (_finish)
        _finish();
}


- (void) stop {
    _next = nil;
    if (_finish) {
        _finish();
        _finish = nil;
    }
}


- (id) _nextRow {
    if (!_next)
        return nil;

    fdb_doc* docinfo;
    uint64_t bodyOffset;
    while(true) {
        fdb_status status = _next(&docinfo, &bodyOffset);
        if (status != FDB_RESULT_SUCCESS || docinfo == NULL) {
            // FDB returns FDB_RESULT_FAIL at end of iteration
            [self stop];
            return nil;
        } else if (_options.skip > 0) {
            // Skip this one
            fdb_doc_free(docinfo);
            --_options.skip;
        } else if (_stopBeforeKey && slicecmp(DataToSlice(_stopBeforeKey),
                                              (slice){docinfo->key, docinfo->keylen})) {
            // Stop before this key, i.e. this is the endKey and inclusiveEnd is false
            [self stop];
            return nil;
        } else if (![_documentClass docInfo: docinfo matchesOptions: &_options]) {
            // Document says skip this (e.g. it's deleted and options say no deleted docs)
            fdb_doc_free(docinfo);
        } else {
            // Whew! Finally found a doc to return...
            break;
        }
    }

    if (_options.limit > 0 && --_options.limit == 0)
        [self stop];

    NSError* error;
    CBForestDocument* doc = [[_documentClass alloc] initWithDB: _db
                                                          info: docinfo
                                                        offset: bodyOffset
                                                       options: _options.contentOptions
                                                         error: &error];
    free(docinfo);
    if (!doc)
        _error = error;
    return doc;
}


- (id) nextObject {
#ifdef PARALLEL
    return [_rows pop];
#else
    return [self _nextRow];
#endif
}


#ifdef PARALLEL
- (void) generateRows {
    while(true) {
        id row;
        do {
            @autoreleasepool {
                row = [self _nextRow];
                if (row)
                    [_rows push: row];
            }
        } while (row);
        [_rows close];
    }
}
#endif


@end
