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


@interface CBForestEnumerator ()
@property (readwrite) NSError* error;
@end


@implementation CBForestEnumerator
{
#ifdef PARALLEL
    @protected
    CBForestDB* _db;
    CBForestEnumerationOptions _options;
    NSData* _stopBeforeKey;
    CBForestQueue* _rows;
#endif
}


@synthesize error=_error;


- (instancetype) initWithDatabase: (CBForestDB*)db
                          options: (const CBForestEnumerationOptions*)options
                           endKey: (NSData*)endKey
{
    self = [super init];
    if (self) {
        _db = db;
        _options = options ? *options : kCBForestEnumerationOptionsDefault;
        if (!_options.inclusiveEnd)
            _stopBeforeKey = endKey;

    }
    return self;
}


- (void) start {
#ifdef PARALLEL
    _rows = [[CBForestQueue alloc] initWithCapacity: PARALLEL_QUEUE_SIZE];
    [self generateRows];
#endif
}


- (id) nextObject {
#ifdef PARALLEL
    return [_rows pop];
#else
    return [self generateRow];
#endif
}


- (id) generateRow {
    return nil; // abstract
}


#ifdef PARALLEL
- (void) generateRows {
    CBForestQueue* rows = _rows;
    __weak CBForestEnumerator* weakSelf = self;
    dispatch_queue_t dispatch = dispatch_queue_create("CBForestDocEnumerator", NULL);
    dispatch_async(dispatch, ^{
        while(true) {
            @autoreleasepool {
                id row = [weakSelf generateRow];
                if (row)
                    [rows push: row];
                else
                    break;
            }
        }
        [rows close];
    });
}
#endif


@end




@implementation CBForestDocEnumerator
{
    CBForestDocEnumeratorNextBlock _next;
    CBForestDocEnumeratorFinishBlock _finish;
    Class _documentClass;
}


- (instancetype) initWithDatabase: (CBForestDB*)db
                          options: (const CBForestEnumerationOptions*)options
                           endKey: (NSData*)endKey
                        nextBlock: (CBForestDocEnumeratorNextBlock)nextBlock
                      finishBlock: (CBForestDocEnumeratorFinishBlock)finishBlock
{
    self = [super initWithDatabase: db options: options endKey: endKey];
    if (self) {
        _documentClass = _db.documentClass;
        _next = nextBlock;
        _finish = finishBlock;
        [self start];
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


- (id) generateRow {
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
        } else if (_stopBeforeKey && 0 == slicecmp(DataToSlice(_stopBeforeKey),
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
        self.error = error;
    return doc;
}


@end
