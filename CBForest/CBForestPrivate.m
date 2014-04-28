//
//  CBForestPrivate.m
//  CBForest
//
//  Created by Jens Alfke on 9/5/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestPrivate.h"
#import "rev_tree.h"


BOOL Check(fdb_status code, NSError** outError) {
    if (code == FDB_RESULT_SUCCESS) {
        if (outError)
            *outError = nil;
        return YES;
    } else {
        static NSString* const kErrorNames[] = {
            nil,
            @"Invalid arguments",
            @"Open failed",
            @"File not found",
            @"Write failed",
            @"Read failed",
            @"Close failed",
            @"Commit failed",
            @"Memory allocation failed",
            @"Key not found",
            @"Database is read-only",
            @"Compaction failed",
            @"Iterator failed",
        };
        NSString* errorName;
        if (code < 0 && -code < (sizeof(kErrorNames)/sizeof(id)))
            errorName = [NSString stringWithFormat: @"ForestDB error: %@", kErrorNames[-code]];
        else if ((int)code
                 == kCBForestErrorDataCorrupt)
            errorName = @"Revision data is corrupted";
        else
            errorName = [NSString stringWithFormat: @"ForestDB error %d", code];
        if (outError) {
            NSDictionary* info = @{NSLocalizedDescriptionKey: errorName};
            *outError = [NSError errorWithDomain: CBForestErrorDomain code: code userInfo: info];
        } else {
            NSLog(@"Warning: CBForest error %d (%@)", code, errorName);
        }
        return NO;
    }
}


slice DataToSlice(NSData* data) {
    return (slice){data.bytes, data.length};
}


slice StringToSlice(NSString* string) {
    return DataToSlice([string dataUsingEncoding: NSUTF8StringEncoding]);
}


NSData* SliceToData(const void* buf, size_t size) {
    if (!buf)
        return nil;
    return [NSData dataWithBytes: buf length: size];
}


NSData* SliceToTempData(const void* buf, size_t size) {
    if (!buf)
        return nil;
    return [NSData dataWithBytesNoCopy: (void*)buf length: size freeWhenDone: NO];
}


NSData* SliceToAdoptingData(const void* buf, size_t size) {
    if (!buf)
        return nil;
    return [NSData dataWithBytesNoCopy: (void*)buf length: size freeWhenDone: YES];
}


NSString* SliceToString(const void* buf, size_t size) {
    if (!buf)
        return nil;
    return [[NSString alloc] initWithBytes: buf
                                    length: size
                                  encoding: NSUTF8StringEncoding];
}


NSData* JSONToData(id obj, NSError** outError) {
    if ([obj isKindOfClass: [NSDictionary class]] || [obj isKindOfClass: [NSArray class]]) {
        return [NSJSONSerialization dataWithJSONObject: obj options: 0 error: outError];
    } else {
        NSArray* array = [[NSArray alloc] initWithObjects: &obj count: 1];
        NSData* data = [NSJSONSerialization dataWithJSONObject: array options: 0 error: outError];
        return [data subdataWithRange: NSMakeRange(1, data.length - 2)];
    }
}


id SliceToJSON(slice buf, NSError** outError) {
    if (buf.size == 0)
        return nil;
    NSCAssert(buf.buf != NULL, @"SliceToJSON(NULL)");
    NSData* data = [[NSData alloc] initWithBytesNoCopy: (void*)buf.buf
                                                length: buf.size
                                          freeWhenDone: NO];
    return DataToJSON(data, outError);
}


void UpdateBuffer(void** outBuf, size_t *outLen, const void* srcBuf, size_t srcLen) {
    free(*outBuf);
    *outBuf = NULL;
    *outLen = srcLen;
    if (srcLen > 0) {
        *outBuf = malloc(srcLen);
        memcpy(*outBuf, srcBuf, srcLen);
    }

}

void UpdateBufferFromData(void** outBuf, size_t *outLen, NSData* data) {
    UpdateBuffer(outBuf, outLen, data.bytes, data.length);
}


#pragma mark - REVISION IDS:


slice CompactRevIDToSlice(NSString* revID) {
    return DataToSlice(CompactRevID(revID));
}


NSData* CompactRevID(NSString* revID) {
    if (!revID)
        return nil;
    //OPT: This is not very efficient.
    slice src = StringToSlice(revID);
    NSMutableData* data = [[NSMutableData alloc] initWithLength: src.size];
    slice dst = DataToSlice(data);
    if (!RevIDCompact(src, &dst))
        return nil; // error
    data.length = dst.size;
    return data;
}


NSString* ExpandRevID(slice compressedRevID) {
    //OPT: This is not very efficient.
    size_t size = RevIDExpandedSize(compressedRevID);
    if (size == 0)
        return SliceToString(compressedRevID.buf, compressedRevID.size);
    NSMutableData* data = [[NSMutableData alloc] initWithLength: size];
    slice buf = DataToSlice(data);
    RevIDExpand(compressedRevID, &buf);
    data.length = buf.size;
    return [[NSString alloc] initWithData: data encoding: NSASCIIStringEncoding];
}


NSUInteger CPUCount(void) {
    __block NSUInteger sCount;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        sCount = [[NSProcessInfo processInfo] processorCount];
    });
    return sCount;
}


@implementation CBForestQueue
{
    NSUInteger _capacity;
    NSMutableArray* _array;
    dispatch_queue_t _q;
    dispatch_semaphore_t _availableCount, _usedCount;
    BOOL _closed;
}

- (instancetype) initWithCapacity: (NSUInteger)capacity {
    self = [super init];
    if (self) {
        _capacity = capacity;
        _array = [[NSMutableArray alloc] initWithCapacity: capacity];
        _q = dispatch_queue_create("CBForestQueue", DISPATCH_QUEUE_SERIAL);
        _availableCount = dispatch_semaphore_create(capacity);
        _usedCount = dispatch_semaphore_create(0);
    }
    return self;
}

- (void)dealloc
{
    //FIX: This is a hacky workaround for dispatch_semaphore's assertion failure
    for (NSUInteger i = _array.count; i>0; i--)
        dispatch_semaphore_signal(_availableCount);
}

- (BOOL) push: (id)value {
    dispatch_semaphore_wait(_availableCount, DISPATCH_TIME_FOREVER);
    __block BOOL success;
    dispatch_sync(_q, ^{
        if (_closed) {
            success = NO;
        } else {
            NSAssert(_array.count < _capacity, @"Queue overflow!");
            [_array addObject: value];
            dispatch_semaphore_signal(_usedCount);
            success = YES;
        }
    });
    return success;
}

- (id) pop {
    dispatch_semaphore_wait(_usedCount, DISPATCH_TIME_FOREVER);
    __block id value;
    dispatch_sync(_q, ^{
        if (_array.count > 0) {
            value = [_array firstObject];
            [_array removeObjectAtIndex: 0];
            dispatch_semaphore_signal(_availableCount);
        } else {
            NSAssert(_closed, @"Queue underflow!");
            value = nil;
            dispatch_semaphore_signal(_usedCount); // so next -pop call won't block
        }
    });
    return value;
}

- (void) close {
    dispatch_sync(_q, ^{
        if (!_closed) {
            _closed = YES;
            dispatch_semaphore_signal(_usedCount); // so that -pop calls will never block
        }
    });
}

@end



@implementation CBForestToken
{
    id _owner;
    NSCondition* _condition;
}

@synthesize name=_name;

- (instancetype)init {
    self = [super init];
    if (self) {
        _condition = [[NSCondition alloc] init];
    }
    return self;
}

- (void) lockWithOwner: (id)owner {
    NSParameterAssert(owner != nil);
    [_condition lock];
    while (_owner != nil && _owner != owner)
        [_condition wait];
    _owner = owner;
    [_condition unlock];
}

- (void) unlockWithOwner: (id)oldOwner {
    NSParameterAssert(oldOwner != nil);
    [_condition lock];
    NSAssert(oldOwner == _owner, @"clearing wrong owner! (%p, expected %p)", oldOwner, _owner);
    _owner = nil;
    [_condition broadcast];
    [_condition unlock];
}
@end
