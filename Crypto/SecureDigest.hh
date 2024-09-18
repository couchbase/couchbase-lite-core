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
#include "Base64.hh"
#include <string>

namespace litecore {

    template <unsigned int N>
    class Hash {
      public:
        /// Stores a digest; returns false if slice is the wrong size
        bool setDigest(fleece::slice);

        /// The digest as a slice
        [[nodiscard]] fleece::slice asSlice() const { return {_bytes, N}; }

        explicit operator fleece::slice() const { return asSlice(); }

        [[nodiscard]] std::string asBase64() const;

        bool operator==(const Hash& x) const { return memcmp(&_bytes, &x._bytes, N) == 0; }

        bool operator!=(const Hash& x) const { return !(*this == x); }

      protected:
        Hash() { memset(_bytes, 0, N); }

        [[nodiscard]] constexpr unsigned int size() const { return N; }

        char _bytes[N]{};
    };

    template <unsigned int N>
    bool Hash<N>::setDigest(fleece::slice s) {
        if ( s.size != size() ) return false;
        memcpy(_bytes, s.buf, size());
        return true;
    }

    template <unsigned int N>
    std::string Hash<N>::asBase64() const {
        return fleece::base64::encode(asSlice());
    }

    /// A SHA-1 digest.
    class SHA1 : public Hash<20> {
      public:
        SHA1() = default;

        /// Constructs instance with a SHA-1 digest of the data in `s`
        explicit SHA1(fleece::slice s) { computeFrom(s); }

        void computeFrom(fleece::slice);

      private:
        friend class SHA1Builder;
    };

    /// Builder for creating SHA-1 digests from piece-by-piece data.
    class SHA1Builder {
      public:
        SHA1Builder();

        /// Add data
        SHA1Builder& operator<<(fleece::slice s);

        /// Add a single byte
        SHA1Builder& operator<<(uint8_t b) { return *this << fleece::slice(&b, 1); }

        /// Finish and write the digest to `result`. (Don't reuse the builder.)
        void finish(void* result, size_t resultSize);

        /// Finish and return the digest as a SHA1 object. (Don't reuse the builder.)
        SHA1 finish() {
            SHA1 result;
            finish(&result._bytes, result.size());
            return result;
        }

      private:
        uint32_t _context[25]{};  // big enough to hold any platform's context struct
    };

    class SHA256 : public Hash<32> {
      public:
        SHA256() = default;

        /// Constructs instance with a SHA-256 digest of the data in `s`
        explicit SHA256(fleece::slice s) { computeFrom(s); }

        void computeFrom(fleece::slice);

      private:
        friend class SHA256Builder;
    };

    /// Builder for creating SHA-1 digests from piece-by-piece data.
    class SHA256Builder {
      public:
        SHA256Builder();

        /// Add data
        SHA256Builder& operator<<(fleece::slice s);

        /// Add a single byte
        SHA256Builder& operator<<(uint8_t b) { return *this << fleece::slice(&b, 1); }

        /// Finish and write the digest to `result`. (Don't reuse the builder.)
        void finish(void* result, size_t resultSize);

        /// Finish and return the digest as a SHA1 object. (Don't reuse the builder.)
        SHA256 finish() {
            SHA256 result;
            finish(&result._bytes, result.size());
            return result;
        }

      private:
        uint32_t _context[28]{};  // big enough to hold any platform's context struct
    };
}  // namespace litecore
