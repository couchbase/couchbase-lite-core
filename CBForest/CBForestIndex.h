//
//  CBForestIndex.h
//  CBForest
//
//  Created by Jens Alfke on 4/1/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBForestDB.h"


typedef struct {
    unsigned skip;
    unsigned limit;
    BOOL inclusiveEnd;
} CBForestQueryParameters;


typedef bool (^CBForestQueryCallbackBlock)(id key, NSString* docID, NSData* rawValue);


@interface CBForestIndex : CBForestDB

- (BOOL) addKeys: (NSArray*)keys
          values: (NSArray*)values
     forDocument: (NSString*)docID
           error: (NSError**)outError;

- (BOOL) queryStartKey: (id)startKey
                endKey: (id)endKey
               options: (const CBForestQueryParameters*)params
                 error: (NSError**)outError
                 block: (CBForestQueryCallbackBlock)block;

@end
