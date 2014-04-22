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
    } else if (outError) {
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
        NSDictionary* info = @{NSLocalizedDescriptionKey: errorName};
        *outError = [NSError errorWithDomain: CBForestErrorDomain code: code userInfo: info];
    }
    return NO;
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
