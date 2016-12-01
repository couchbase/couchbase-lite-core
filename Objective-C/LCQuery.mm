//
//  LCQuery.mm
//  LiteCore
//
//  Created by Jens Alfke on 11/30/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#import "LCQuery.h"
#import "LC_Internal.h"
#import "StringBytes.hh"
#import "c4DBQuery.h"


template <typename T>
static inline T* _Nonnull  assertNonNull(T* _Nullable t) {
    NSCAssert(t != nil, @"Unexpected nil value");
    return (T*)t;
}


@interface LCQueryRow ()
- (instancetype) initWithDatabase: (LCDatabase*)db
                  queryEnumerator: (C4QueryEnumerator*)e;
@end

@interface LCQueryEnumerator : NSEnumerator
- (instancetype) initWithDatabase: (LCDatabase*)db
                  queryEnumerator: (C4QueryEnumerator*)e;
@end


@implementation LCQuery
{
    LCDatabase* _db;
    C4Query* _c4Query;
}

@synthesize skip=_skip, limit=_limit, parameters=_parameters;


- (instancetype) initWithDatabase: (LCDatabase*)db
                            where: (NSDictionary*)where
                          orderBy: (nullable NSArray*)sortDescriptors
                            error: (NSError**)outError
{
    NSParameterAssert(db != nil);
    NSParameterAssert(where != nil);
    self = [super init];
    if (self) {
        _db = db;
        _limit = UINT64_MAX;

        NSData* whereData = [NSJSONSerialization dataWithJSONObject: where options: 0
                                                              error: outError];
        if (!whereData)
            return nil;

        NSData* sortData = nil;
        if (sortDescriptors) {
            NSMutableArray* sorts = [NSMutableArray new];
            for (id sd in sortDescriptors) {
                if ([sd isKindOfClass: [NSString class]])
                    [sorts addObject: sd];
                else {
                    NSSortDescriptor* sort = sd;
                    NSString* key = sort.key;
                    if (!sort.ascending)
                        key = [key stringByAppendingString: @"-"];
                    [sorts addObject: key];
                }
            }
            sortData = [NSJSONSerialization dataWithJSONObject: sorts options: 0
                                                         error: NULL];
        }

        C4Error c4Err;
        _c4Query = c4query_new(_db.c4db,
                               {whereData.bytes, whereData.length},
                               {sortData.bytes, sortData.length},
                               &c4Err);
        if (!_c4Query) {
            convertError(c4Err, outError);
            return nil;
        }
    }
    return self;
}


- (void) dealloc {
    c4query_free(_c4Query);
}


- (NSEnumerator<LCQueryRow*>*) run: (NSError**)outError {
    C4QueryOptions options = kC4DefaultQueryOptions;
    options.skip = _skip;
    options.limit = _limit;
    NSData* paramJSON = nil;
    if (_parameters) {
        paramJSON = [NSJSONSerialization dataWithJSONObject: (NSDictionary*)_parameters
                                                    options: 0
                                                      error: outError];
        if (!paramJSON)
            return nil;
    }
    C4Error c4Err;
    auto e = c4query_run(_c4Query, &options, {paramJSON.bytes, paramJSON.length}, &c4Err);
    if (!e) {
        convertError(c4Err, outError);
        return nil;
    }
    return [[LCQueryEnumerator alloc] initWithDatabase: _db queryEnumerator: e];
}

@end




@implementation LCQueryEnumerator
{
    LCDatabase* _db;
    C4QueryEnumerator* _c4enum;
}

- (instancetype) initWithDatabase: (LCDatabase*)db
                  queryEnumerator: (C4QueryEnumerator*)e
{
    self = [super init];
    if (self) {
        _db = db;
        _c4enum = e;
    }
    return self;
}


- (void) dealloc {
    c4queryenum_free(_c4enum);
}


- (LCQueryRow*) nextObject {
    C4Error err;
    if (c4queryenum_next(_c4enum, &err)) {
        return [[LCQueryRow alloc] initWithDatabase: _db queryEnumerator: _c4enum];
    } else if (err.code) {
        C4Warn("LCQueryEnumerator error: %d/%d", err.domain, err.code);
        return nil;
    } else {
        return nil;
    }
}


@end




@implementation LCQueryRow
{
    LCDatabase* _db;
}

@synthesize documentID=_documentID, sequence=_sequence;


- (instancetype) initWithDatabase: (LCDatabase*)db
                  queryEnumerator: (C4QueryEnumerator*)e
{
    self = [super init];
    if (self) {
        _db = db;
        _documentID = assertNonNull( [[NSString alloc] initWithBytes: e->docID.buf
                                                              length: e->docID.size
                                                            encoding: NSUTF8StringEncoding] );
        _sequence = e->docSequence;
    }
    return self;
}


- (LCDocument*) document {
    return assertNonNull( [_db documentWithID: _documentID] );
}


@end
