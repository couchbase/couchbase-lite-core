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
    }
    if (outError) {
        NSDictionary* info = nil;
#if 0
        const char* str = couchstore_strerror(code);
        if (str) {
            NSString* description = [NSString stringWithUTF8String: str];
            if (description)
                info = @{ NSLocalizedDescriptionKey : description };
        }
#endif
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


void UpdateBuffer(void** outBuf, size_t *outLen, NSData* data) {
    free(*outBuf);
    *outBuf = NULL;
    *outLen = data.length;
    if (*outLen > 0) {
        *outBuf = malloc(*outLen);
        memcpy(*outBuf, data.bytes, *outLen);
    }

}