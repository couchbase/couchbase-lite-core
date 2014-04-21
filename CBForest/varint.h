//
//  varint.h
//  CBForest
//
//  Created by Jens Alfke on 3/31/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef CBForest_varint_h
#define CBForest_varint_h

#include <stddef.h>
#include <stdbool.h>
#include "sized_buf.h"

// Based on varint implementation from the Go language (src/pkg/encoding/binary/varint.go)

// This file implements "varint" encoding of 64-bit integers.
// The encoding is:
// - unsigned integers are serialized 7 bits at a time, starting with the
//   least significant bits
// - the most significant bit (msb) in each output byte indicates if there
//   is a continuation byte (msb = 1)
// - signed integers are mapped to unsigned integers using "zig-zag"
//   encoding: Positive values x are written as 2*x + 0, negative values
//   are written as 2*(^x) + 1; that is, negative numbers are complemented
//   and whether to complement is encoded in bit 0.
//
// Design note:
// At most 10 bytes are needed for 64-bit values. The encoding could
// be more dense: a full 64-bit value needs an extra byte just to hold bit 63.
// Instead, the msb of the previous byte could be used to hold bit 63 since we
// know there can't be more than 64 bits. This is a trivial improvement and
// would reduce the maximum encoding length to 9 bytes. However, it breaks the
// invariant that the msb is always the "continuation bit" and thus makes the
// format incompatible with a varint encoding for larger numbers (say 128-bit).


/** MaxVarintLenN is the maximum length of a varint-encoded N-bit integer. */
enum {
    kMaxVarintLen16 = 3,
    kMaxVarintLen32 = 5,
    kMaxVarintLen64 = 10,
};

/** Returns the number of bytes needed to encode a specific integer. */
size_t SizeOfVarInt(uint64_t n);

/** Encodes n as a varint, writing it to buf. Returns the number of bytes written. */
size_t PutUVarInt(void *buf, uint64_t n);

/** Decodes a varint from the bytes in buf, storing it into *n.
    Returns the number of bytes read, or 0 if the data is invalid (buffer too short or number
    too long.) */
size_t GetUVarInt(sized_buf buf, uint64_t *n);

/** Decodes a varint from buf, and advances buf to the remaining space after it.
    Returns false if the end of the buffer is reached or there is a parse error. */
bool ReadUVarInt(sized_buf *buf, uint64_t *n);

/** Encodes a varint into buf, and advances buf to the remaining space after it.
    Returns false if there isn't enough room. */
bool WriteUVarInt(sized_buf *buf, uint64_t n);

#endif
