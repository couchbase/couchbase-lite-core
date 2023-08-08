//
// RevID.hh
//
// Copyright 2014-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Base.hh"

namespace litecore {
    class Version;
    class VersionVector;

    /** A compressed revision ID in binary form.
        Since this is based on `slice`, it doesn't own the memory it points to.
        For an owning version, see \ref revidBuffer.

        There are two types of revision IDs: digests and versions.

        **Digest form** is/was used by the revision-tree versioning system.
        It consists of a generation count and an MD5 or SHA-1 digest.
        - The ASCII form looks like "123-cafebabedeadbeefdeadfade".
        - The binary form consists of the generation as a varint, followed by the digest as raw binary.

        **Version form** is used by the version-vector system in [the upcoming] Couchbase Mobile 3.x.
        It consists of a logical timestamp and a UUID "source ID" (or "peer ID".)
        - An all-zero source ID (`kMeSourceID`) is reserved to mean "the local device/database".
        - The ASCII form combines a hex timestamp with a base64 source ID, separated by an `@`,
          for example `1772c7cb27da0000@ZegpoldZegpoldZegpoldA`.
          The source ID zero is represented as a '*' character.
        - The binary form is, basically, a zero byte, the timestamp as a varint, and the source.
          The leading zero is to distinguish it from the digest form. It's actually a bit more
          complicated, to shave off a few bytes; see the comment in Version.cc.
        The version form is also represented by the `Version` class.

        PLEASE NOTE: A revid in version form can store an entire version vector, since that format
        just consists of multiple binary versions concatenated.
        HOWEVER, the `revid` API only gives information about the first (current) version in the
        vector, except for the `asVersionVector` method. */
    class revid : public slice {
      public:
        revid() : slice() {}

        revid(const void* b, size_t s) : slice(b, s) {}

        explicit revid(slice s) : slice(s) {}

        bool operator<(const revid&) const FLPURE;

        bool operator>(const revid& r) const FLPURE { return r < *this; }

        /// Returns true if both revids represent the same revision:
        /// - If both are version vectors (or single versions) and their leading versions are equal
        /// - or if both are digest-based and are bitwise equal.
        [[nodiscard]] bool isEquivalentTo(const revid&) const noexcept FLPURE;

        /// Returns true for version-vector style (time@peer), false for rev-tree style (gen-digest).
        [[nodiscard]] bool isVersion() const noexcept FLPURE { return size > 0 && (*this)[0] == 0; }

        //---- Tree revision IDs only
        [[nodiscard]] pair<unsigned, slice> generationAndDigest() const FLPURE;
        [[nodiscard]] unsigned              generation() const FLPURE;

        [[nodiscard]] slice digest() const FLPURE { return generationAndDigest().second; }

        //---- Version IDs only
        [[nodiscard]] Version       asVersion() const FLPURE;
        [[nodiscard]] VersionVector asVersionVector() const;

        //---- ASCII conversions:
        [[nodiscard]] alloc_slice expanded() const;
        [[nodiscard]] bool        expandInto(slice_ostream& dst) const noexcept;
        [[nodiscard]] std::string str() const;

        explicit operator std::string() const { return str(); }

        friend class revidBuffer;
    };

    /** A wrapper around revid that owns the buffer for the revid.

        PLEASE NOTE: the `parse` and `tryParse` methods can parse a single version, but not an entire
        VersionVector -- they will barf at the first comma. This is intentional. A `revidBuffer` is fixed-
        size and can't hold an arbitrarily long version vector. */
    class revidBuffer {
      public:
        revidBuffer() : _revid(&_buffer, 0) {}

        revidBuffer(unsigned generation, slice digest);

        explicit revidBuffer(revid rev) { *this = rev; }

        explicit revidBuffer(const Version& v) { *this = v; }

        revidBuffer(const revidBuffer& r) { *this = r; }

        /** Constructs a revidBuffer from an ASCII revision (digest or version style).
            Throws BadRevisionID if the string isn't parseable.*/
        explicit revidBuffer(slice asciiString) : _revid(&_buffer, 0) { parse(asciiString); }

        [[nodiscard]] const revid& getRevID() const { return _revid; }

        explicit operator revid() const { return _revid; }

        revidBuffer& operator=(const revidBuffer&) noexcept;
        revidBuffer& operator=(const revid&);
        revidBuffer& operator=(const Version& vers) noexcept;

        /** Parses a regular ASCII revID (digest or version style) and compresses it.
            Throws BadRevisionID if the string isn't parseable.
            \warning This will not parse an entire version vector, only its first component!
                    To parse the entire vector, call \ref VersionVector::fromASCII. */
        void parse(slice asciiString);

        /** Parses a regular ASCII revID (digest or version style) and compresses it.
            Returns false if the string isn't parseable.
            \warning This will not parse an entire version vector, only its first component!
                    To parse the entire vector, call \ref VersionVector::fromASCII. */
        [[nodiscard]] bool tryParse(slice asciiString) noexcept;

      private:
        uint8_t _buffer[42]{};
        revid   _revid;
    };
}  // namespace litecore
