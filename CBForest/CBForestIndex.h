//
//  CBForestIndex.h
//  CBForest
//
//  Created by Jens Alfke on 4/1/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBForestDB.h"
@class CBForestQueryEnumerator;


/** Use this in the values passed to -setKeys:values:forDocument:error: to indicate no value */
extern id kCBForestIndexNoValue;


typedef void (^CBForestIndexEmitBlock)(id key, id value);


/** Callback block for a CBForestIndex query. */
typedef void (^CBForestQueryCallbackBlock)(id key,
                                           NSData* valueJSON,
                                           NSString* docID,
                                           CBForestSequence sequence,
                                           BOOL *stop);


/** Key/value index for a database, stored as a separate database file. */
@interface CBForestIndex : CBForestDB

- (void) updateForDocument: (NSString*)docID
                atSequence: (CBForestSequence)docSequence
                   addKeys: (void(^)(CBForestIndexEmitBlock))addKeysBlock;

@end


/** An enumerator over an index.
    The -nextObject method advances the iterator and returns the current key, or nil at the end.
    The corresponding value, document ID and sequence can be gotten from properties. */
@interface CBForestQueryEnumerator : NSEnumerator

/** Queries an index for a contiguous range of keys. */
- (instancetype) initWithIndex: (CBForestIndex*)index
                      startKey: (id)startKey
                    startDocID: (NSString*)startDocID
                        endKey: (id)endKey
                      endDocID: (NSString*)endDocID
                       options: (const CBForestEnumerationOptions*)options
                         error: (NSError**)outError;

/** Queries an index for a set of keys. */
- (instancetype) initWithIndex: (CBForestIndex*)index
                          keys: (NSEnumerator*)keys
                       options: (const CBForestEnumerationOptions*)options
                         error: (NSError**)outError;

@property (readonly, nonatomic) id key, value;
@property (readonly, nonatomic) NSData* valueData;
@property (readonly, nonatomic) NSString* docID;
@property (readonly, nonatomic) CBForestSequence sequence;
@property (readonly, nonatomic) NSError* error;

@end


/** An index enumerator that finds documents containing all or some of the given keys.
    The object returned by -nextObject is the document ID. */
@interface CBForestQueryMultiKeyEnumerator : NSEnumerator

- (instancetype) initWithIndex: (CBForestIndex*)index
                          keys: (NSArray*)keys
                  intersection: (BOOL)intersection
                         error: (NSError**)outError;
@end
