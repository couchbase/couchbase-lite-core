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
#define PARALLEL_QUEUE_SIZE 16
#define BATCH_SIZE 16


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


- (void) run {
#ifdef PARALLEL
    _rows = [[CBForestQueue alloc] initWithCapacity: PARALLEL_QUEUE_SIZE];
    CBForestQueue* rows = _rows;
    dispatch_queue_t dispatch = dispatch_queue_create("CBForestDocEnumerator", NULL);
    dispatch_async(dispatch, ^{
        [self generateRows];
        [rows close];
    });
#endif
}


- (id) nextObject {
#ifdef PARALLEL
    return [_rows pop];
#else
    return [self generateRow];
#endif
}


#ifdef PARALLEL
- (void) generateRows {
    // abstract
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
        [self run];
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


- (void) generateRows {
    fdb_status status;
    do {
        fdb_doc* docs[BATCH_SIZE];
        uint64_t bodyOffsets[BATCH_SIZE];
        unsigned n = BATCH_SIZE;
        if (!_next)
            break;
        status = _next(&n, docs, bodyOffsets);

        for (unsigned i=0; i<n; i++) {
            fdb_doc *docinfo = docs[i];
            if (_options.skip > 0) {
                // Skip this one
                --_options.skip;
            } else if (_stopBeforeKey && 0 == slicecmp(DataToSlice(_stopBeforeKey),
                                                       (slice){docinfo->key, docinfo->keylen})) {
                // Stop before this key, i.e. this is the endKey and inclusiveEnd is false
                status = FDB_RESULT_KEY_NOT_FOUND;
                break;
            } else if ([_documentClass docInfo: docinfo matchesOptions: &_options]) {
                // Whew! Finally found a doc to return...
                NSError* error;
                CBForestDocument* doc = [[_documentClass alloc] initWithDB: _db
                                                                      info: docinfo
                                                                    offset: bodyOffsets[i]
                                                                   options: _options.contentOptions
                                                                     error: &error];
                free(docinfo);
                docs[i] = NULL; // Document object adopts doc's key/meta/body, so don't free them
                if (doc) {
                    [_rows push: doc];

                    if (_options.limit > 0 && --_options.limit == 0) {
                        // That's enough rows; time to stop
                        status = FDB_RESULT_KEY_NOT_FOUND;
                        break;
                    }
                } else {
                    self.error = error;
                    status = FDB_RESULT_KEY_NOT_FOUND;
                    break;
                }
            }
        }

        for (unsigned i=0; i<n; i++)
            if (docs[i])
                fdb_doc_free(docs[i]);

    } while (status == FDB_RESULT_SUCCESS);
    [self stop];
}


@end



@implementation CBForestReverseEnumerator
{
    NSEnumerator* _baseEnumerator;
}

- (id) initWithEnumerator: (CBForestEnumerator*)e {
    self = [super init];
    if (self) {
        //FIX: //OPT: Inefficient. Hopefully ForestDB will implement support for revers iteration.
        _baseEnumerator = e.allObjects.reverseObjectEnumerator;
        self.error = e.error;
    }
    return self;
}


- (id) nextObject {
    return _baseEnumerator.nextObject;
}


@end