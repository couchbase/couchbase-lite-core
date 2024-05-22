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
#include "fleece/function_ref.hh"
#include "fleece/Fleece.hh"
#include "fleece/slice.hh"

C4_ASSUME_NONNULL_BEGIN

struct C4BlobKey;

namespace fleece::impl {
    class Dict;
    class SharedKeys;
}  // namespace fleece::impl

namespace litecore::legacy_attachments {

    /** Returns true if this is the name of a 1.x metadata property ("_id", "_rev", etc.) */
    bool isOldMetaProperty(fleece::slice key);

    /** Returns true if the document contains 1.x metadata properties (at top level). */
    bool hasOldMetaProperties(fleece::Dict root);
    bool hasOldMetaProperties(const fleece::impl::Dict* root);

    /** Encodes to Fleece, without any 1.x metadata properties.
        The _attachments property is treated specially, in that any entries in it that don't
        appear elsewhere in the dictionary as blobs are preserved. */
    fleece::alloc_slice encodeStrippingOldMetaProperties(fleece::Dict root, fleece::SharedKeys);
    fleece::alloc_slice encodeStrippingOldMetaProperties(const fleece::impl::Dict* root,
                                                         fleece::impl::SharedKeys* C4NULLABLE);

    /** Returns true if the document body contains blob or attachment references.
        @param noBlobs  If true, finds only old-style attachments.
        @returns  True if blobs/attachments were found. */
    bool hasBlobReferences(fleece::Dict root, bool noBlobs);

    /** Callback for `findBlobReferences`. */
    using FindBlobCallback = fleece::function_ref<void(FLDeepIterator, fleece::Dict blob, const C4BlobKey& key)>;

    /** Finds blob or attachment references in a document body, invoking the callback for each.
        @param root  The root of the parsed document.
        @param unique  If true, skips multiple references to the same blob digest.
        @param noBlobs  If true, finds only old-style attachments.
        @param callback  The callback. */
    void findBlobReferences(fleece::Dict root, bool unique, bool noBlobs, const FindBlobCallback& callback);

    /** Detects whether the iterator's current value is a blob/attachment.
        @param i  The iterator.
        @param blobKey  The blob key will be copied here.
        @param noBlobs  If true, finds only old-style attachments.
        @return  True if this is a blob/attachment. */
    bool isBlobOrAttachment(FLDeepIterator i, C4BlobKey* blobKey, bool noBlobs);

    /** Writes the doc body `root` to the encoder `enc`. If it contains blobs, equivalent entries
        will be encoded to the legacy `_attachments` property.
        @param enc  A Fleece or JSON encoder.
        @param root  The document body to encode.
        @param revpos  If nonzero, will be written as a `revpos` property of converted blob refs. */
    void encodeRevWithLegacyAttachments(fleece::Encoder& enc, fleece::Dict root, unsigned revpos);

}  // namespace litecore::legacy_attachments

C4_ASSUME_NONNULL_END
