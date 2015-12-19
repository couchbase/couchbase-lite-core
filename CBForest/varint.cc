//
//  varint.cc
//  CBForest
//
//  Created by Jens Alfke on 3/31/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "varint.hh"
#include <stdio.h>
#include "slice.hh"


namespace cbforest {

size_t SizeOfVarInt(uint64_t n) {
    size_t size = 1;
    while (n >= 0x80) {
        size++;
        n >>= 7;
    }
    return size;
}


size_t PutUVarInt(void *buf, uint64_t n) {
    uint8_t* dst = (uint8_t*)buf;
    while (n >= 0x80) {
        *dst++ = (n & 0xFF) | 0x80;
        n >>= 7;
    }
    *dst++ = (uint8_t)n;
    return dst - (uint8_t*)buf;
}


size_t GetUVarInt(slice buf, uint64_t *n) {
    uint64_t result = 0;
    int shift = 0;
    for (int i = 0; i < buf.size; i++) {
        uint8_t byte = ((const uint8_t*)buf.buf)[i];
        result |= (uint64_t)(byte & 0x7f) << shift;
        if (byte >= 0x80) {
            shift += 7;
        } else {
            if (i > 9 || (i == 9 && byte > 1))
                return 0; // Numeric overflow
            *n = result;
            return i + 1;
        }
    }
    return 0; // buffer too short
}


bool ReadUVarInt(slice *buf, uint64_t *n) {
    if (buf->size == 0)
        return false;
    size_t bytesRead = GetUVarInt(*buf, n);
    if (bytesRead == 0)
        return false;
    buf->moveStart(bytesRead);
    return true;
}


bool WriteUVarInt(slice *buf, uint64_t n) {
    if (buf->size < kMaxVarintLen64 && buf->size < SizeOfVarInt(n))
        return false;
    size_t bytesWritten = PutUVarInt((void*)buf->buf, n);
    buf->moveStart(bytesWritten);
    return true;
}

}