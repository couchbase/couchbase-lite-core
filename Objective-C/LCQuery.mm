//
//  LCQuery.mm
//  LiteCore
//
//  Created by Jens Alfke on 11/30/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#import "LCQuery.h"
#import "LCQuery+Predicates.h"
#import "LC_Internal.h"
#import "StringBytes.hh"
#import "c4Document.h"
#import "c4DBQuery.h"
#import "Fleece.h"


template <typename T>
static inline T* _Nonnull  assertNonNull(T* _Nullable t) {
    NSCAssert(t != nil, @"Unexpected nil value");
    return (T*)t;
}


@interface LCQuery ()
@property (readonly, nonatomic) C4Query* c4query;
@end

@interface LCQueryRow ()
- (instancetype) initWithQuery: (LCQuery*)query enumerator: (C4QueryEnumerator*)e;
@end

@interface LCQueryEnumerator : NSEnumerator
- (instancetype) initWithQuery: (LCQuery*)query enumerator: (C4QueryEnumerator*)e;
@end


@implementation LCQuery

@synthesize database=_db, skip=_skip, limit=_limit, parameters=_parameters, c4query=_c4Query;


- (nullable instancetype) initWithDatabase: (LCDatabase*)db
                                     query: (nullable id)query
                                     error: (NSError**)error
{
    return [self initWithDatabase: db where: query orderBy: nil error: error];
}


- (instancetype) initWithDatabase: (LCDatabase*)db
                            where: (id)where
                          orderBy: (nullable NSArray*)sortDescriptors
                            error: (NSError**)outError
{
    self = [super init];
    if (self) {
        NSData* jsonData = [[self class] encodeQuery: where orderBy: sortDescriptors error: outError];
        if (!jsonData)
            return nil;
        NSLog(@"Query encoded as %@",
              [[NSString alloc] initWithData: jsonData encoding: NSUTF8StringEncoding]);
        C4Error c4Err;
        _db = db;
        _c4Query = c4query_new(db.c4db, {jsonData.bytes, jsonData.length}, &c4Err);
        if (!_c4Query) {
            convertError(c4Err, outError);
            return nil;
        }
        _limit = UINT64_MAX;
    }
    return self;
}


+ (NSData*) encodeQuery: (id)where
                orderBy: (nullable NSArray*)sortDescriptors
                  error: (NSError**)outError
{
    id whereJSON = nil;
    if (where) {
        if ([where isKindOfClass: [NSArray class]] || [where isKindOfClass: [NSDictionary class]]) {
            whereJSON = where;
        } else if ([where isKindOfClass: [NSPredicate class]]) {
            whereJSON = [self encodePredicate: where error: outError];
        } else if ([where isKindOfClass: [NSString class]]) {
            where = [NSPredicate predicateWithFormat: (NSString*)where argumentArray: nil];
            if (where)
                whereJSON = [self encodePredicate: where error: outError];
            else
                mkError(outError, @"Invalid predicate string");
        } else if (where != nil) {
            mkError(outError, @"Invalid 'where' parameter type %@ for LCQuery", [where class]);
        }
        if (!whereJSON)
            return nil;
    }

    NSMutableDictionary* q;
    if ([whereJSON isKindOfClass: [NSDictionary class]]) {
        q = [whereJSON mutableCopy];
    } else {
        q = [NSMutableDictionary new];
        if (whereJSON)
            q[@"WHERE"] = whereJSON;
    }
    if (sortDescriptors) {
        NSMutableArray* sorts = [NSMutableArray new];
        for (id sd in sortDescriptors) {
            id key;
            if ([sd isKindOfClass: [NSString class]]) {
                if ([sd hasPrefix: @"-"])
                    key = @[@"DESC", [sd substringFromIndex: 1]];
                else
                    key = sd;
            } else {
                NSSortDescriptor* sort = sd;
                key = sort.key;
                if (!sort.ascending)
                    key = @[@"DESC", key];
            }
            [sorts addObject: key];
        }
        q[@"ORDER BY"] = sorts;
    }

    return [NSJSONSerialization dataWithJSONObject: q options: 0 error: outError];
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
    return [[LCQueryEnumerator alloc] initWithQuery: self enumerator: e];
}

@end




@implementation LCQueryEnumerator
{
    LCQuery *_query;
    C4QueryEnumerator* _c4enum;
}

- (instancetype) initWithQuery: (LCQuery*)query enumerator: (C4QueryEnumerator*)e
{
    self = [super init];
    if (self) {
        _query = query;
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
        return [[LCQueryRow alloc] initWithQuery: _query enumerator: _c4enum];
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
    LCQuery *_query;
    C4FullTextTerm* _matches;
}

@synthesize documentID=_documentID, sequence=_sequence, matchCount=_matchCount;


- (instancetype) initWithQuery: (LCQuery*)query enumerator: (C4QueryEnumerator*)e {
    self = [super init];
    if (self) {
        _query = query;
        _documentID = assertNonNull( [[NSString alloc] initWithBytes: e->docID.buf
                                                              length: e->docID.size
                                                            encoding: NSUTF8StringEncoding] );
        _sequence = e->docSequence;
        _matchCount = e->fullTextTermCount;
        if (_matchCount > 0) {
            _matches = new C4FullTextTerm[_matchCount];
            memcpy(_matches, e->fullTextTerms, _matchCount * sizeof(C4FullTextTerm));
        }
    }
    return self;
}


- (void) dealloc {
    delete [] _matches;
}


- (LCDocument*) document {
    return assertNonNull( [_query.database documentWithID: _documentID] );
}


// Full-text search:


- (NSData*) fullTextUTF8Data {
    stringBytes docIDSlice(_documentID);
    C4SliceResult text = c4query_fullTextMatched(_query.c4query, docIDSlice, _sequence, nullptr);
    if (!text.buf)
        return nil;
    NSData *result = [NSData dataWithBytes: text.buf length: text.size];
    c4slice_free(text);
    return result;
}


- (NSString*) fullTextMatched {
    NSData* data = self.fullTextUTF8Data;
    return data ? [[NSString alloc] initWithData: data encoding: NSUTF8StringEncoding] : nil;
}


- (NSUInteger) termIndexOfMatch: (NSUInteger)matchNumber {
    NSParameterAssert(matchNumber < _matchCount);
    return _matches[matchNumber].termIndex;
}

- (NSRange) textRangeOfMatch: (NSUInteger)matchNumber {
    NSParameterAssert(matchNumber < _matchCount);
    NSUInteger byteStart  = _matches[matchNumber].start;
    NSUInteger byteLength = _matches[matchNumber].length;
    NSData* rawText = self.fullTextUTF8Data;
    if (!rawText)
        return NSMakeRange(NSNotFound, 0);
    return NSMakeRange(charCountOfUTF8ByteRange(rawText.bytes, 0, byteStart),
                       charCountOfUTF8ByteRange(rawText.bytes, byteStart, byteStart + byteLength));
}


// Determines the number of NSString (UTF-16) characters in a byte range of a UTF-8 string. */
static NSUInteger charCountOfUTF8ByteRange(const void* bytes, NSUInteger byteStart, NSUInteger byteEnd) {
    if (byteStart == byteEnd)
        return 0;
    NSString* prefix = [[NSString alloc] initWithBytesNoCopy: (UInt8*)bytes + byteStart
                                                      length: byteEnd - byteStart
                                                    encoding: NSUTF8StringEncoding
                                                freeWhenDone: NO];
    return prefix.length;
}


@end
