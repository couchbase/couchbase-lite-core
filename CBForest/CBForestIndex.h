//
//  CBForestIndex.h
//  CBForest
//
//  Created by Jens Alfke on 4/1/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBForestDB.h"


/** Use this in the values passed to -setKeys:values:forDocument:error: to indicate no value */
extern id kCBForestIndexNoValue;


/** Callback block for a CBForestIndex query. */
typedef void (^CBForestQueryCallbackBlock)(id key,
                                           NSString* docID,
                                           id value,
                                           BOOL *stop);


/** Key/value index for a database, stored as a separate database file. */
@interface CBForestIndex : CBForestDB

/** Stores the given key/value pairs associated with the given document.
    Any previously stored pairs for the document will be removed. */
- (BOOL) setKeys: (NSArray*)keys
          values: (NSArray*)values
     forDocument: (NSString*)docID
           error: (NSError**)outError;

/** Queries the index, calling the block once for each result. */
- (BOOL) queryStartKey: (id)startKey
                endKey: (id)endKey
               options: (const CBForestEnumerationOptions*)options
                 error: (NSError**)outError
                 block: (CBForestQueryCallbackBlock)block;

@end
