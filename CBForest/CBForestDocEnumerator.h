//
//  CBForestDocEnumerator.h
//  CBForest
//
//  Created by Jens Alfke on 4/23/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <CBForest/CBForestDB.h>
#import <forestdb.h>


// INTERNAL HEADER!

typedef fdb_status (^CBForestDocEnumeratorNextBlock)(fdb_doc**, uint64_t *bodyOffset);
typedef void (^CBForestDocEnumeratorFinishBlock)();

@interface CBForestDocEnumerator : CBForestEnumerator

- (instancetype) initWithDatabase: (CBForestDB*)db
                          options: (const CBForestEnumerationOptions*)options
                           endKey: (NSData*)endKey
                        nextBlock: (CBForestDocEnumeratorNextBlock)nextBlock
                      finishBlock: (CBForestDocEnumeratorFinishBlock)finishBlock;

@end
