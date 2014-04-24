//
//  CBForestDocEnumerator.h
//  CBForest
//
//  Created by Jens Alfke on 4/23/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBForestDB.h"
#import <forestdb.h>


typedef fdb_status (^CBForestDocEnumeratorNextBlock)(fdb_doc**, uint64_t *bodyOffset);
typedef void (^CBForestDocEnumeratorFinishBlock)();


@interface CBForestDocEnumerator : NSEnumerator

- (instancetype) init; // empty enumerator

- (instancetype) initWithDatabase: (CBForestDB*)db
                          options: (const CBForestEnumerationOptions*)options
                        nextBlock: (CBForestDocEnumeratorNextBlock)nextBlock
                      finishBlock: (CBForestDocEnumeratorFinishBlock)finishBlock;

/** If this is set, enumeration will stop and return nil when this key is reached. */
@property NSData* stopBeforeKey;

@property (readonly) NSError* error;

@end
