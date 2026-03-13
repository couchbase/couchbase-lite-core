//
// c4Document.hh
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
#include "c4DocumentTypes.h"
#include "c4DocumentStruct.h"
#include "fleece/FLBase.h"

#if !LITECORE_CPP_API
#    error "c4DocumentTypes.h was included before Base.hh"
#endif

C4_ASSUME_NONNULL_BEGIN

// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


enum class RevIDType {
    Invalid,
    Tree,
    Version,
};

struct C4Document
    : public fleece::RefCounted
    , C4Base {
    // NOTE: Instances are created with database->getDocument or database->putDocument.

    /// Creates a new instance identical to this one, except its `extraInfo` is unset.
    virtual Ref<C4Document> copy() const = 0;

    // Accessors:

    C4DocumentFlags flags() const noexcept FLPURE { return _flags; }

    const alloc_slice& docID() const noexcept FLPURE { return _docID; }

    const alloc_slice& revID() const noexcept FLPURE { return _revID; }

    C4SequenceNumber sequence() const noexcept FLPURE { return _sequence; }

    const C4Revision& selectedRev() const noexcept FLPURE { return _selected; }

    /// The C C4Document struct. Using the accessors above is preferred.
    const C4Document_C& pub() const { return *(C4Document_C*)&_flags; }

    C4ExtraInfo& extraInfo() { return _extraInfo; }

    const C4ExtraInfo& extraInfo() const { return _extraInfo; }

    C4Collection* collection() const;
    C4Database*   database() const;

    virtual bool exists() const = 0;

    virtual bool revisionsLoaded() const noexcept = 0;

    [[nodiscard]] virtual bool loadRevisions() const = 0;

    virtual bool loadRevisionBody() const = 0;  // can throw; returns false if compacted away

    virtual bool hasRevisionBody() const noexcept = 0;

    virtual slice getRevisionBody() const noexcept = 0;

    virtual FLDict getProperties() const noexcept;

    alloc_slice bodyAsJSON(bool canonical = false) const;

    // Selecting revisions:

    virtual bool selectCurrentRevision() noexcept                  = 0;
    virtual bool selectRevision(slice revID, bool withBody = true) = 0;  // returns false if not found

    virtual bool selectParentRevision() noexcept { failUnsupported(); }

    virtual bool selectNextRevision()                                              = 0;
    virtual bool selectNextLeafRevision(bool includeDeleted, bool withBody = true) = 0;

    virtual bool selectCommonAncestorRevision(slice revID1, slice revID2) { failUnsupported(); }

    /// Returns true if `ancestor` is an ancestor of (or equal to) `rev`. (Rev trees only)
    bool revisionHasAncestor(slice rev, slice ancestor);

    // Revision info:

    virtual alloc_slice getSelectedRevIDGlobalForm() const;

    virtual alloc_slice getRevisionHistory(unsigned    maxHistory,
                                           const slice backToRevs[C4NULLABLE],  // nullable if count=0
                                           unsigned    backToRevsCount) const {
        failUnsupported();
    }

    /// Returns true if `revID` is known to be a direct ancestor of (or equal to) the current revision.
    /// @note In a version-vector document, `revID` may be an entire version vector.
    virtual bool currentRevDescendsFrom(slice revID) const = 0;

    // Remote database revision tracking:

    virtual alloc_slice remoteAncestorRevID(C4RemoteID)                 = 0;
    virtual void        setRemoteAncestorRevID(C4RemoteID, slice revID) = 0;
    virtual bool        isRevRejected()                                 = 0;
    virtual void        revIsRejected(slice revID)                      = 0;

    // Purging:

    virtual bool removeRevisionBody() noexcept { return false; }

    virtual int32_t purgeRevision(slice revid) { failUnsupported(); }

    // Conflicts:

    // pruneLosingBranch is not exposed to the API, so it will probably always be true
    void resolveConflict(slice winningRevID, slice losingRevID, FLDict C4NULLABLE mergedProperties,
                         C4RevisionFlags mergedFlags, bool pruneLosingBranch = true);

    virtual void resolveConflict(slice winningRevID, slice losingRevID, slice mergedBody, C4RevisionFlags mergedFlags,
                                 bool pruneLosingBranch = true) {
        failUnsupported();
    }

    // Updating & Saving:

    /** Adds a new revision to this document in the database, and returns a
        new document instance that has the new revision.
        If the database already contains a conflicting revision, returns nullptr. */
    virtual Retained<C4Document> update(slice revBody, C4RevisionFlags) const;

    /** Saves changes to the document. Returns false on conflict. */
    virtual bool save(unsigned maxRevTreeDepth = 0) = 0;

    // Static utility functions:

    static alloc_slice createDocID();

    static constexpr size_t    kGeneratedIDLength = 23;
    [[nodiscard]] static char* generateID(char* outDocID, size_t bufferSize) noexcept;

    static constexpr size_t   kMaxDocIDLength = 240;
    [[nodiscard]] static bool isValidDocID(slice) noexcept;
    static void               requireValidDocID(slice);  // throws kC4ErrorBadDocID

    [[nodiscard]] static RevIDType typeOfRevID(slice) noexcept;
    static void                    requireValidRevID(slice);  // throws kC4ErrorBadRevisionID
    static bool                    equalRevIDs(slice revID1, slice revID2) noexcept;
    static unsigned                getRevIDGeneration(slice revID) noexcept;
    static uint64_t                getRevIDTimestamp(slice revID) noexcept;
    static alloc_slice             legacyRevIDAsVersion(slice revID) noexcept;

    static C4RevisionFlags revisionFlagsFromDocFlags(C4DocumentFlags docFlags) noexcept;
    static C4DocumentFlags documentFlagsFromRevFlags(C4RevisionFlags revFlags) noexcept;

    /// Returns the Document instance, if any, that contains the given Fleece value.
    static C4Document* C4NULLABLE containingValue(FLValue) noexcept;

    [[nodiscard]] static bool isOldMetaProperty(slice propertyName) noexcept;
    [[nodiscard]] static bool hasOldMetaProperties(FLDict) noexcept;

    static alloc_slice encodeStrippingOldMetaProperties(FLDict properties, FLSharedKeys);

    // Special property names & values:

    /** The Dict property that identifies it as a special type of object.
        For example, a blob is represented as `{"@type":"blob", "digest":"xxxx", ...}` */
    static constexpr slice kObjectTypeProperty = "@type";

    /** Value of `kC4ObjectTypeProperty` that denotes an encryptable value. */
    static constexpr slice kObjectType_Encryptable = "encryptable";

    /** Encryptable-value property containing the actual value; may be any JSON/Fleece type.
        Required if `ciphertext` is absent. */
    static constexpr slice kValueToEncryptProperty = "value";

    /** Encryptable-value property containing the encrypted data as a Base64-encoded string.
        Required if `value` is absent. */
    static constexpr slice kCiphertextProperty = "ciphertext";

    // NOTE: Blob-related constants are defined in c4BlobStore.hh.

  protected:
    friend class litecore::DatabaseImpl;
    friend class litecore::CollectionImpl;
    friend class litecore::Upgrader;

    C4Document(C4Collection*, alloc_slice docID_);
    C4Document(const C4Document&);
    ~C4Document() override;

    litecore::KeyStore& keyStore() const;

    [[noreturn]] static void failUnsupported();

    void setRevID(litecore::revid);

    void clearSelectedRevision() noexcept;

    /// Subroutine of \ref CollectionImpl::putDocument that adds a revision with an existing revID,
    /// i.e. one pulled by the replicator.
    /// To avoid throwing exceptions in normal circumstances, this function will return common
    /// errors via an `outError` parameter. It can also throw exceptions for unexpected errors.
    /// @param rq  The put request
    /// @param outError  "Expected" errors like Conflict or Not Found will be stored here.
    /// @return the index (in rq.history) of the common ancestor; or -1 on error.
    virtual int32_t putExistingRevision(const C4DocPutRequest& rq, C4Error* C4NULLABLE outError) = 0;

    /// Subroutine of \ref CollectionImpl::putDocument and \ref C4Document::update that adds a new
    /// revision, i.e. when saving a document.
    /// To avoid throwing exceptions in normal circumstances, this function will return common
    /// errors via an `outError` parameter. It can also throw exceptions for unexpected errors.
    /// @param rq  The put request
    /// @param outError  "Expected" errors like Conflict or Not Found will be stored here.
    /// @return  True on success, false on error
    virtual bool putNewRevision(const C4DocPutRequest& rq, C4Error* C4NULLABLE outError) = 0;

    /// Subroutine of \ref update that sanity checks the parameters before trying to save.
    bool checkNewRev(slice parentRevID, C4RevisionFlags flags, bool allowConflict, C4Error* C4NULLABLE) noexcept;

    // (These fields must have the same offset and layout as the corresponding fields in the C
    // struct C4Document_C declared in c4DocumentStruct.h. See full explanation in c4Document.cc.)
    alignas(void*)                     // <--very important
            C4DocumentFlags _flags;    // Document flags
    alloc_slice      _docID;           // Document ID
    alloc_slice      _revID;           // Revision ID of current revision
    C4SequenceNumber _sequence;        // Sequence at which doc was last updated
    C4Revision       _selected;        // Describes the currently-selected revision
    C4ExtraInfo      _extraInfo = {};  // For client use
    // (end of fields that have to match C4Document_C)

    alloc_slice               _selectedRevID;  // Backing store for _selected.revID
    litecore::CollectionImpl* _collection;     // Owning collection
};

C4_ASSUME_NONNULL_END
