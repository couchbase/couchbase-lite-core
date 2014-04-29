//
//  CBEnumerator.m
//  CBForest
//
//  Created by Jens Alfke on 4/28/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBEnumerator.h"


@interface CBForestEnumerator : NSEnumerator
@end


@implementation CBForestEnumerator
{
    CBEnumeratorBlock _block;
}

- (instancetype) initWithBlock: (CBEnumeratorBlock)block {
    self = [super init];
    if (self) {
        _block = block;
    }
    return self;
}

- (id) nextObject {
    return _block();
}

@end


NSArray* CBEnumeratorAllObjects(CBEnumeratorBlock block) {
    NSMutableArray* allObjects = [[NSMutableArray alloc] init];
    while(YES) {
        id next = block();
        if (!next)
            break;
        [allObjects addObject: next];
    }
    return allObjects;
}


NSEnumerator* CBEnumeratorBlockToObject(CBEnumeratorBlock block) {
    return [[CBForestEnumerator alloc] initWithBlock: block];
}


NSEnumerator* CBEnumeratorBlockReversedToObject(CBEnumeratorBlock block) {
    return CBEnumeratorAllObjects(block).reverseObjectEnumerator;
}



CBEnumeratorBlock CBBufferedEnumerator(NSUInteger bufferSize, CBEnumeratorBlock e) {
    if (bufferSize == 0)
        return e;
    
    NSMutableArray* buffer = [[NSMutableArray alloc] initWithCapacity: bufferSize];
    dispatch_queue_t queue = dispatch_queue_create("BufferedEnumerator", DISPATCH_QUEUE_SERIAL);
    dispatch_semaphore_t hasObjectsSem = dispatch_semaphore_create(0);

    void (^produce)(NSUInteger) = ^(NSUInteger n) {
        dispatch_async(queue, ^{
            for (NSUInteger i=0; i<n; i++) {
                id next = e();
                if (next) {
                    @synchronized(buffer) {
                        [buffer addObject: next];
                    }
                }
                dispatch_semaphore_signal(hasObjectsSem);
                if (!next)
                    break;
            }
        });
    };

    produce(bufferSize);

    return ^id() {
        dispatch_semaphore_wait(hasObjectsSem, DISPATCH_TIME_FOREVER);
        id result = nil;
        @synchronized(buffer) {
            if (buffer.count > 0) {
                result = buffer[0];
                [buffer removeObjectAtIndex: 0];
            }
        }
        if (result)
            produce(1);
        else
            dispatch_semaphore_signal(hasObjectsSem); // so next call doesn't block
        return result;
    };
}
