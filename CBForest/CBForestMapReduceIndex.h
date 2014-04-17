//
//  CBForestMapReduceIndex.h
//  CBForest
//
//  Created by Jens Alfke on 4/4/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBForestIndex.h"


typedef void (^CBForestIndexEmitBlock)(id key, id value);
typedef void (^CBForestIndexMapBlock)(CBForestDocument* doc,
                                      CBForestIndexEmitBlock emit);


/** An index that uses a map function to process documents from a source database. */
@interface CBForestMapReduceIndex : CBForestIndex

@property CBForestDB* sourceDatabase;
@property (copy) CBForestIndexMapBlock map;
@property (nonatomic, copy) NSString* mapVersion;

@property (readonly) CBForestSequence lastSequenceIndexed;

- (BOOL) updateIndex: (NSError**)outError;

@end
