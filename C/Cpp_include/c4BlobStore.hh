//
// c4BlobStore.hh
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
#include "c4Base.hh"
#include "c4BlobStoreTypes.h"
#include "c4DatabaseTypes.h"
#include "fleece/function_ref.hh"
#include "fleece/FLBase.h"
#include "fleece/InstanceCounted.hh"
#include <memory>
#include <optional>
#include <unordered_set>

C4_ASSUME_NONNULL_BEGIN

// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


namespace C4Blob {
    using slice       = fleece::slice;
    using alloc_slice = fleece::alloc_slice;

    // (this is an alias of `C4Document::kObjectTypeProperty`)
    extern const slice kObjectTypeProperty;

    /** Value of `C4Document::kObjectTypeProperty` ("@type") that denotes a blob. */
    static constexpr slice kObjectType_Blob = "blob";

    /** Blob dict property containing a digest of the contents. (Required if "data" is absent) */
    static constexpr slice kDigestProperty = "digest";

    /** Blob dict property containing the contents,
        as a Fleece data value (preferred) or a base64-encoded string.
        (Required if "digest" is absent) */
    static constexpr slice kDataProperty = "data";

    /** Blob dict property containing the length in bytes of the contents. (Required.) */
    static constexpr slice kLengthProperty = "length";

    /** Blob dict property containing the MIME type of the contents (optional). */
    static constexpr slice kContentTypeProperty = "content_type";

    /** Top-level document property whose value is a CBL 1.x / CouchDB attachments container. */
    static constexpr slice kLegacyAttachmentsProperty = "_attachments";


    /** Returns true if the given dictionary is a [reference to a] blob.
        This tests whether it contains a "@type" property whose value is "blob". */
    bool isBlob(FLDict C4NULLABLE dict);

    /** Returns true if the given dictionary is an old-style attachment in the document.
        This tests whether `inDocument` contains an `_attachments` property, whose value is a Dict,
        and that one of that Dict's values is the given `dict`. */
    bool isAttachmentIn(FLDict dict, FLDict inDocument);

    /** Returns the dict's "digest" property decoded into a C4BlobKey.
        Returns `nullopt` if the digest is missing or invalid.
        \note This does not check if the dict itself is a blob, just reads the "digest" prop. */
    std::optional<C4BlobKey> keyFromDigestProperty(FLDict dict);

    /** Guesses whether the blob's content is likely to be compressible,
        based on the MIME type in the Dict's "content_type" property.
        (Returns false if that property is not present.) */
    bool isLikelyCompressible(FLDict C4NULLABLE);

    /** Returns true if this dict (usually the root of a document) contains any blobs within. */
    bool dictContainsBlobs(FLDict C4NULLABLE) noexcept;

    /** A callback informing the caller of a blob or attachment.
        @param blobMeta  The blob/attachment's metadata Dict.
        @returns  True to continue iterating, false to return ASAP. */
    using FindBlobCallback = fleece::function_ref<bool(FLDict blobMeta)>;

    /** Finds all blob references in a Fleece Dict, recursively. */
    bool findBlobReferences(FLDict C4NULLABLE, const FindBlobCallback&);

    /** Finds old-style attachment references, i.e. sub-dictionaries of "_attachments". */
    bool findAttachmentReferences(FLDict C4NULLABLE docRoot, const FindBlobCallback& callback);
};  // namespace C4Blob

struct C4ReadStream
    : public fleece::InstanceCounted
    , C4Base {
    C4ReadStream(const C4BlobStore&, C4BlobKey);
    C4ReadStream(C4ReadStream&&) noexcept;
    ~C4ReadStream() override;
    size_t                 read(void* buffer, size_t maxBytes);
    [[nodiscard]] uint64_t getLength() const;
    void                   seek(uint64_t pos);

  private:
    std::unique_ptr<litecore::SeekableReadStream> _impl;
};

struct C4WriteStream
    : public fleece::InstanceCounted
    , C4Base {
    explicit C4WriteStream(C4BlobStore&);
    C4WriteStream(C4WriteStream&&) noexcept;
    ~C4WriteStream() override;

    [[nodiscard]] C4BlobStore& blobStore() const { return _store; }

    void                   write(slice);
    [[nodiscard]] uint64_t getBytesWritten() const noexcept;
    C4BlobKey              computeBlobKey();
    C4BlobKey              install(const C4BlobKey* C4NULLABLE expectedKey = nullptr);

  private:
    std::unique_ptr<litecore::BlobWriteStream> _impl;
    C4BlobStore&                               _store;
};

struct C4BlobStore : C4Base {
    // NOTE: Usually accessed via database->getBlobStore().

    ~C4BlobStore();

    [[nodiscard]] bool isEncrypted() const { return _encryptionKey.algorithm != kC4EncryptionNone; }

    void deleteStore();

    /// The size of the blob in bytes. Returns -1 if there is no such blob.
    [[nodiscard]] int64_t getSize(C4BlobKey) const;

    /// The blob's data. Throws an exception if there is no such blob.
    [[nodiscard]] alloc_slice getContents(C4BlobKey) const;

    /// The filesystem path of a blob, or nullslice if no blob with that key exists.
    [[nodiscard]] alloc_slice getFilePath(C4BlobKey) const;

    C4BlobKey createBlob(slice contents, const C4BlobKey* C4NULLABLE expectedKey = nullptr);
    void      deleteBlob(C4BlobKey);

    [[nodiscard]] C4ReadStream openReadStream(C4BlobKey key) const { return {*this, key}; }

    C4WriteStream openWriteStream() { return C4WriteStream(*this); }

    /** Returns the contents of a blob referenced by a dict. Inline data will be decoded if
         necessary, or the "digest" property will be looked up in the BlobStore if one is
         provided.
         Returns a null slice if the blob data is not inline but no BlobStore is given.
         Otherwise throws an exception if it's unable to return data. */
    alloc_slice getBlobData(FLDict dict) const;

    // Used internally by C4Database:
    unsigned deleteAllExcept(const std::unordered_set<C4BlobKey>& inUse);
    void     copyBlobsTo(C4BlobStore&);
    void     replaceWith(C4BlobStore&);

    // rarely used / for testing only:
    C4BlobStore(slice dirPath, C4DatabaseFlags, const C4EncryptionKey& = {});

    C4BlobStore(const C4BlobStore&) = delete;

  protected:
    friend struct C4ReadStream;
    friend struct C4WriteStream;

    [[nodiscard]] litecore::FilePath                            dir() const;
    [[nodiscard]] litecore::FilePath                            pathForKey(C4BlobKey) const;
    [[nodiscard]] std::unique_ptr<litecore::SeekableReadStream> getReadStream(C4BlobKey) const;
    std::unique_ptr<litecore::BlobWriteStream>                  getWriteStream();
    C4BlobKey install(litecore::BlobWriteStream*, const C4BlobKey* C4NULLABLE expectedKey);

  private:
    std::string const _dirPath;
    C4DatabaseFlags   _flags;
    C4EncryptionKey   _encryptionKey;
};

namespace std {
    // Declares the default hash function for `C4BlobKey`
    template <>
    struct hash<C4BlobKey> {
        std::size_t operator()(C4BlobKey const& key) const { return fleece::slice(key).hash(); }
    };
}  // namespace std

C4_ASSUME_NONNULL_END
