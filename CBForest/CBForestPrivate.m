//
//  CBForestPrivate.m
//  CBForest
//
//  Created by Jens Alfke on 9/5/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import "CBForestPrivate.h"


BOOL Check(fdb_status code, NSError** outError) {
    if (code == FDB_RESULT_SUCCESS) {
        if (outError)
            *outError = nil;
        return YES;
    } else if (outError) {
        static NSString* const kErrorNames[] = {nil, @"Error", @"Invalid arguments"};
        NSDictionary* info = @{NSLocalizedDescriptionKey: kErrorNames[code]};
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


NSString* BufToString(const void* buf, size_t size) {
    if (!buf)
        return nil;
    return [[NSString alloc] initWithBytes: buf
                                    length: size
                                  encoding: NSUTF8StringEncoding];
}


sized_buf CopyBuf(sized_buf buf) {
    if (buf.size > 0) {
        void* newBuf = malloc(buf.size);
        memcpy(newBuf, buf.buf, buf.size);
        buf.buf = newBuf;
    }
    return buf;
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