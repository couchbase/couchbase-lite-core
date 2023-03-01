//
// c4BlobStoreTypes.h
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.h"
#ifdef __cplusplus
#    include "fleece/slice.hh"
#    include <optional>
#    include <string>
#endif

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/** \defgroup Blobs Blobs
    @{ */

//////// BLOB KEYS:

/** \name Blob Keys
    @{ */

/** A unique identifier of a blob based on a SHA-1 digest of its contents. */
struct C4BlobKey {
    uint8_t bytes[20];

#ifdef __cplusplus
    using slice = fleece::slice;

    /** Generates a SHA-1 digest of the content data and returns it as a C4BlobKey. */
    static C4BlobKey computeDigestOfContent(slice content);

    /** Translates an ASCII blob key, as found in a blob's "digest" property, to a C4BlobKey.
        Returns `nullopt` if invalid. */
    static std::optional<C4BlobKey> withDigestString(slice base64);

    /** Returns the ASCII form, as used in a blob's "digest" property. */
    std::string digestString() const;

    /** Returns a slice pointing to the digest bytes. */
    explicit operator slice() const { return slice(bytes, sizeof(bytes)); }

    bool operator==(const C4BlobKey& k) const { return memcmp(bytes, k.bytes, sizeof(bytes)) == 0; }

    bool operator!=(const C4BlobKey& k) const { return !(*this == k); }
#endif
};

/** @} */
/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
