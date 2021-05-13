//
// c4BlobStoreTypes.h
//
// Copyright (c) 2021 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "c4Base.h"
#ifdef __cplusplus
    #include "fleece/slice.hh"
    #include <optional>
    #include <string>
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
    explicit operator slice() const     {return slice(bytes, sizeof(bytes));}

    bool operator== (const C4BlobKey &k) const {
        return memcmp(bytes, k.bytes, sizeof(bytes)) == 0;
    }

    bool operator!= (const C4BlobKey &k) const {
        return !(*this == k);
    }
#endif
};


/** @} */
/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
