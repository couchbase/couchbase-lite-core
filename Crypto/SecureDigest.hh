//
// SecureDigest.hh
//
// Copyright 2015-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/slice.hh"
#include <string>

namespace litecore {

    /// A SHA-1 digest.
    class SHA1 {
    public:
        SHA1()                               { memset(bytes, 0, sizeof(bytes)); }

        /// Constructs instance with a SHA-1 digest of the data in `s`
        explicit SHA1(fleece::slice s)       {computeFrom(s);}

        /// Computes a SHA-1 digest of the data
        void computeFrom(fleece::slice);

        /// Stores a digest; returns false if slice is the wrong size
        bool setDigest(fleece::slice);

        /// The digest as a slice
        fleece::slice asSlice() const         {return {bytes, sizeof(bytes)};}
        operator fleece::slice() const        {return asSlice();}

        std::string asBase64() const;

        bool operator==(const SHA1 &x) const  {return memcmp(&bytes, &x.bytes, sizeof(bytes)) == 0;}
        bool operator!= (const SHA1 &x) const {return !(*this == x);}

    private:
        char bytes[20];

        friend class SHA1Builder;
    };


    /// Builder for creating SHA-1 digests from piece-by-piece data.
    class SHA1Builder {
    public:
        SHA1Builder();

        /// Add a single byte
        SHA1Builder& operator<< (fleece::slice s);

        /// Add data
        SHA1Builder& operator<< (uint8_t b)     {return *this << fleece::slice(&b, 1);}

        /// Finish and write the digest to `result`. (Don't reuse the builder.)
        void finish(void *result, size_t resultSize);

        /// Finish and return the digest as a SHA1 object. (Don't reuse the builder.)
        SHA1 finish() {
            SHA1 result;
            finish(&result.bytes, sizeof(result.bytes));
            return result;
        }

    private:
        uint8_t _context[100];  // big enough to hold any platform's context struct
    };


}


