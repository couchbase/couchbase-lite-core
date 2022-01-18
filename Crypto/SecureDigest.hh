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
#include <array>
#include <string>

namespace litecore {

    enum DigestType {
        SHA,
    };


    /// A cryptographic digest. Available instantiations are <SHA,1> and <SHA,256>.
    /// (SHA384 and SHA512 could be added by doing some copy-and-pasting in the .cc file.)
    template <DigestType TYPE, size_t SIZE>
    class Digest {
    public:
        class Builder;

        Digest()                               {_bytes.fill(std::byte{0});}

        /// Constructs instance with a digest of the data in `s`.
        explicit Digest(fleece::slice s)       {computeFrom(s);}

        inline Digest(Builder&&);

        /// Computes a digest of the data.
        void computeFrom(fleece::slice data);

        /// Stores a digest; returns false if slice is the wrong size.
        bool setDigest(fleece::slice digestData);

        /// The digest as a slice.
        fleece::slice asSlice() const         {return {_bytes.data(), _bytes.size()};}
        operator fleece::slice() const        {return asSlice();}

        /// The digest encoded in Base64.
        std::string asBase64() const;

        bool operator==(const Digest &x) const   {return _bytes == x._bytes;}
        bool operator!=(const Digest &x) const   {return _bytes != x._bytes;}

    private:
        static constexpr size_t kSizeInBytes = ((TYPE == SHA && SIZE == 1) ? 160 : SIZE) / 8;

        std::array<std::byte,kSizeInBytes> _bytes;
    };


    /// Builder for creating digests incrementally from piece-by-piece data.
    template <DigestType TYPE, size_t SIZE>
    class Digest<TYPE, SIZE>::Builder {
    public:
        Builder();

        /// Adds data.
        Builder& operator<< (fleece::slice s);

        /// Adds a single byte.
        Builder& operator<< (uint8_t b)     {return *this << fleece::slice(&b, 1);}

        /// Finishes and writes the digest.
        /// @warning Don't reuse this builder.
        /// @param result  The address to write the digest to.
        /// @param resultSize  Must be equal to the size of a digest.
        void finish(void *result, size_t resultSize);

        /// Finishes and returns the digest as a new object.
        /// @warning Don't reuse this builder.
        Digest finish()                     {return Digest(std::move(*this));}

    private:
        std::byte _context[110];  // big enough to hold any platform's context struct
    };


    template<DigestType TYPE, size_t SIZE>
    Digest<TYPE, SIZE>::Digest(Builder &&builder) {
        builder.finish(_bytes.data(), _bytes.size());
    }

    template <DigestType TYPE, size_t SIZE>
    void Digest<TYPE,SIZE>::computeFrom(fleece::slice s) {
        (Builder() << s).finish(_bytes.data(), _bytes.size());
    }


    // Shorthand names:
    using SHA1        = Digest<SHA,1>;
    using SHA1Builder = Digest<SHA,1>::Builder;

    using SHA256      = Digest<SHA,256>;
}


