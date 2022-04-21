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
#include "c4Base.h"
#include "fleece/function_ref.hh"
#include "fleece/slice.hh"
#include "fleece/Fleece.h"

C4_ASSUME_NONNULL_BEGIN

/** Utilities for dealing with 'legacy' properties like _id, _rev, _deleted, _attachments. */
namespace litecore::legacy_attachments {

    /** Returns true if this is the name of a 1.x metadata property ("_id", "_rev", etc.) */
    bool isOldMetaProperty(fleece::slice key);

    /** Returns true if the document contains 1.x metadata properties (at top level). */
    bool hasOldMetaProperties(FLDict root);

    /** Encodes to Fleece, without any 1.x metadata properties.
        The _attachments property is treated specially, in that any entries in it that don't
        appear elsewhere in the dictionary as blobs are preserved. */
    fleece::alloc_slice encodeStrippingOldMetaProperties(FLDict root,
                                                         FLSharedKeys C4NULLABLE);

    using FindBlobCallback = fleece::function_ref<void(FLDeepIterator,
                                                       FLDict blob,
                                                       const C4BlobKey &key)>;

    /** Finds all blob references in the dict, at any depth. */
    void findBlobReferences(FLDict root,
                            bool unique,
                            const FindBlobCallback &callback,
                            bool attachmentsOnly =false);

    /** Writes `root` to the encoder, transforming blobs into old-school `_attachments` dict */
    void encodeRevWithLegacyAttachments(FLEncoder enc,
                                        FLDict root,
                                        unsigned revpos);
}

C4_ASSUME_NONNULL_END
