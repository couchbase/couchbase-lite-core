//
// LegacyAttachments.hh
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Compat.h"
#include "fleece/slice.hh"

C4_ASSUME_NONNULL_BEGIN

namespace fleece::impl {
    class Dict;
    class SharedKeys;
}  // namespace fleece::impl

namespace litecore::legacy_attachments {

    /** Returns true if this is the name of a 1.x metadata property ("_id", "_rev", etc.) */
    bool isOldMetaProperty(fleece::slice key);

    /** Returns true if the document contains 1.x metadata properties (at top level). */
    bool hasOldMetaProperties(const fleece::impl::Dict* root);

    /** Encodes to Fleece, without any 1.x metadata properties.
            The _attachments property is treated specially, in that any entries in it that don't
            appear elsewhere in the dictionary as blobs are preserved. */
    fleece::alloc_slice encodeStrippingOldMetaProperties(const fleece::impl::Dict* root,
                                                         fleece::impl::SharedKeys* C4NULLABLE);
}  // namespace litecore::legacy_attachments

C4_ASSUME_NONNULL_END
