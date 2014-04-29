//
//  CBEnumerator.h
//  CBForest
//
//  Created by Jens Alfke on 4/28/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <Foundation/Foundation.h>


typedef id (^CBEnumeratorBlock)(void);


NSArray* CBEnumeratorAllObjects(CBEnumeratorBlock block);

NSEnumerator* CBEnumeratorBlockToObject(CBEnumeratorBlock);
NSEnumerator* CBEnumeratorBlockReversedToObject(CBEnumeratorBlock);


CBEnumeratorBlock CBBufferedEnumerator(NSUInteger bufferSize, CBEnumeratorBlock e);
