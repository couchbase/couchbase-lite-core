//
//  CRC32Writer.hh
//  LiteCore
//
//  Created by Jens Alfke on 11/1/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "fleece/slice.hh"
#include "crc32c.h"

namespace litecore {

    /** A simple output stream that just computes a CRC32 digest of the data.
        (This is informally compatible with Writer, enough to serve as the template parameter
        of JSONEncoderTo.)*/
    class CRC32Writer {
    public:
        explicit CRC32Writer(size_t initialDigest =0)
        :_digest(uint32_t(initialDigest))
        { }

        uint32_t digest() const {return _digest;}

        void reset() {_digest = 0;}

        CRC32Writer& operator<< (slice s) {
            _digest = crc32c((const uint8_t*)s.buf, s.size, _digest);
            return *this;
        }

        CRC32Writer& operator<< (uint8_t byte) {
            _digest = crc32c((const uint8_t*)&byte, 1, _digest);
            return *this;
        }

        void writeBase64(slice data) {
            std::string b64 = data.base64String();
            *this << slice(b64.data(), b64.size());
        }

    private:
        uint32_t _digest {0};
    };

}
