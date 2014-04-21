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


sized_buf DataToBuf(NSData* data) {
    sized_buf buf = {(char*)data.bytes, data.length};
    return buf;
}


sized_buf StringToBuf(NSString* string) {
    return DataToBuf([string dataUsingEncoding: NSUTF8StringEncoding]);
}


NSData* BufToData(const void* buf, size_t size) {
    if (!buf)
        return nil;
    return [NSData dataWithBytes: buf length: size];
}


NSData* BufToTempData(const void* buf, size_t size) {
    if (!buf)
        return nil;
    return [NSData dataWithBytesNoCopy: (void*)buf length: size freeWhenDone: NO];
}


NSString* BufToString(const void* buf, size_t size) {
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


id BufToJSON(sized_buf buf, NSError** outError) {
    if (buf.size == 0)
        return nil;
    NSCAssert(buf.buf != NULL, @"BufToJSON(NULL)");
    NSData* data = [[NSData alloc] initWithBytesNoCopy: buf.buf length: buf.size freeWhenDone: NO];
    return DataToJSON(data, outError);
}


sized_buf CopyBuf(sized_buf buf) {
    if (buf.size > 0) {
        void* newBuf = malloc(buf.size);
        memcpy(newBuf, buf.buf, buf.size);
        buf.buf = newBuf;
    }
    return buf;
}


int CompareBufs(sized_buf a, sized_buf b) {
    size_t minSize = a.size < b.size ? a.size : b.size;
    int result = memcmp(a.buf, b.buf, minSize);
    if (result == 0) {
        if (a.size < b.size)
            result = -1;
        else if (a.size > b.size)
            result = 1;
    }
    return result;
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






sized_buf CompactRevIDToBuf(NSString* revID) {
    return DataToBuf(CompactRevID(revID));
}


NSData* CompactRevID(NSString* revID) {
    if (!revID)
        return nil;
    //OPT: This is not very efficient.
    sized_buf src = StringToBuf(revID);
    NSMutableData* data = [[NSMutableData alloc] initWithLength: src.size];
    sized_buf dst = DataToBuf(data);
    if (!RevIDCompact(src, &dst))
        return nil; // error
    data.length = dst.size;
    return data;
}


NSString* ExpandRevID(sized_buf compressedRevID) {
    //OPT: This is not very efficient.
    size_t size = RevIDExpandedSize(compressedRevID);
    if (size == 0)
        return BufToString(compressedRevID.buf, compressedRevID.size);
    NSMutableData* data = [[NSMutableData alloc] initWithLength: size];
    sized_buf buf = DataToBuf(data);
    RevIDExpand(compressedRevID, &buf);
    data.length = buf.size;
    return [[NSString alloc] initWithData: data encoding: NSASCIIStringEncoding];
}
