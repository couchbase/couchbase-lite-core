//
// c4Document.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
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
#include "c4Base.hh"
#include "c4DocumentTypes.h"
#include "c4DocumentStruct.h"
#include "function_ref.hh"
#include "fleece/Fleece.h"

#if ! LITECORE_CPP_API
#    error "c4DocumentTypes.h was included before Base.hh"
#endif

C4_ASSUME_NONNULL_BEGIN


// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


struct C4Document : public fleece::RefCounted,
                    C4Base
{
    // NOTE: Instances are created with database->getDocument or database->putDocument.

    /// Creates a new instance identical to this one, except its `extraInfo` is unset.
    virtual Retained<C4Document> copy() const =0;

    // Accessors:

    C4DocumentFlags     flags() const noexcept FLPURE       {return _flags;}
    const alloc_slice&  docID() const noexcept FLPURE       {return _docID;}
    const alloc_slice&  revID() const noexcept FLPURE       {return _revID;}
    C4SequenceNumber    sequence() const noexcept FLPURE    {return _sequence;}
    const C4Revision&   selectedRev() const noexcept FLPURE {return _selected;}

    /// The C C4Document struct. Using the accessors above is preferred.
    const C4Document_C& pub() const                         {return *(C4Document_C*)&_flags;}

    C4ExtraInfo& extraInfo()                                {return _extraInfo;}
    const C4ExtraInfo& extraInfo() const                    {return _extraInfo;}

    C4Collection* collection() const;
    C4Database* database() const;

    virtual bool exists() const =0;

    virtual bool revisionsLoaded() const noexcept =0;

    virtual bool loadRevisions() const MUST_USE_RESULT =0;

    virtual bool loadRevisionBody() const =0; // can throw; returns false if compacted away

    virtual bool hasRevisionBody() const noexcept =0;

    virtual slice getRevisionBody() const noexcept =0;

    virtual FLDict getProperties() const noexcept;

    alloc_slice bodyAsJSON(bool canonical =false) const;

    // Selecting revisions:

    virtual bool selectCurrentRevision() noexcept =0;
    virtual bool selectRevision(slice revID, bool withBody =true) =0;   // returns false if not found
    virtual bool selectParentRevision() noexcept                            {failUnsupported();}
    virtual bool selectNextRevision() =0;
    virtual bool selectNextLeafRevision(bool includeDeleted, bool withBody =true) =0;
    virtual bool selectCommonAncestorRevision(slice revID1, slice revID2)   {failUnsupported();}

    // Revision info:

    virtual alloc_slice getSelectedRevIDGlobalForm() const;

    virtual alloc_slice getRevisionHistory(unsigned maxHistory,
                                           const slice backToRevs[C4NULLABLE], // nullable if count=0
                                           unsigned backToRevsCount) const        {failUnsupported();}

    // Remote database revision tracking:

    virtual alloc_slice remoteAncestorRevID(C4RemoteID) =0;
    virtual void setRemoteAncestorRevID(C4RemoteID, slice revID) =0;

    // Purging:

    virtual bool removeRevisionBody() noexcept                              {return false;}

    virtual int32_t purgeRevision(slice revid)                            {failUnsupported();}

    // Conflicts:

    void resolveConflict(slice winningRevID,
                         slice losingRevID,
                         FLDict C4NULLABLE mergedProperties,
                         C4RevisionFlags mergedFlags,
                         bool pruneLosingBranch =true);


    virtual void resolveConflict(slice winningRevID,
                                 slice losingRevID,
                                 slice mergedBody,
                                 C4RevisionFlags mergedFlags,
                                 bool pruneLosingBranch =true)              {failUnsupported();}

    // Updating & Saving:

    /** Adds a new revision to this document in the database, and returns a
        new document instance that has the new revision.
        If the database already contains a conflicting revision, returns nullptr. */
    virtual Retained<C4Document> update(slice revBody, C4RevisionFlags) const;

    /** Saves changes to the document. Returns false on conflict. */
    virtual bool save(unsigned maxRevTreeDepth =0) =0;


    // Static utility functions:

    static alloc_slice createDocID();

    static constexpr size_t kGeneratedIDLength = 23;
    static char* generateID(char *outDocID, size_t bufferSize) noexcept;

    static constexpr size_t kMaxDocIDLength = 240;
    static bool isValidDocID(slice) noexcept;
    static void requireValidDocID(slice);       // throws kC4ErrorBadDocID

    static bool equalRevIDs(slice revID1,
                            slice revID2) noexcept;
    static unsigned getRevIDGeneration(slice revID) noexcept;

    static C4RevisionFlags revisionFlagsFromDocFlags(C4DocumentFlags docFlags) noexcept;

    /// Returns the Document instance, if any, that contains the given Fleece value.
    static C4Document* C4NULLABLE containingValue(FLValue) noexcept;

    static bool isOldMetaProperty(slice propertyName) noexcept;
    static bool hasOldMetaProperties(FLDict) noexcept;

    static alloc_slice encodeStrippingOldMetaProperties(FLDict properties,
                                                        FLSharedKeys);

protected:
    friend class litecore::DatabaseImpl;
    friend class litecore::CollectionImpl;
    friend class litecore::Upgrader;

    C4Document(C4Collection*, alloc_slice docID_);
    C4Document(const C4Document&);
    virtual ~C4Document();

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
    virtual int32_t putExistingRevision(const C4DocPutRequest &rq, C4Error* C4NULLABLE outError) =0;

    /// Subroutine of \ref CollectionImpl::putDocument and \ref C4Document::update that adds a new
    /// revision, i.e. when saving a document.
    /// To avoid throwing exceptions in normal circumstances, this function will return common
    /// errors via an `outError` parameter. It can also throw exceptions for unexpected errors.
    /// @param rq  The put request
    /// @param outError  "Expected" errors like Conflict or Not Found will be stored here.
    /// @return  True on success, false on error
    virtual bool putNewRevision(const C4DocPutRequest &rq, C4Error* C4NULLABLE outError) =0;

    /// Subroutine of \ref update that sanity checks the parameters before trying to save.
    bool checkNewRev(slice parentRevID,
                     C4RevisionFlags flags,
                     bool allowConflict,
                     C4Error* C4NULLABLE) noexcept;

    // (These fields must have the same offset and layout as the corresponding fields in the C
    // struct C4Document_C declared in c4DocumentStruct.h. See full explanation in c4Document.cc.)
    alignas(void*) // <--very important
    C4DocumentFlags           _flags;           // Document flags
    alloc_slice               _docID;           // Document ID
    alloc_slice               _revID;           // Revision ID of current revision
    C4SequenceNumber          _sequence;        // Sequence at which doc was last updated
    C4Revision                _selected;        // Describes the currently-selected revision
    C4ExtraInfo               _extraInfo = {};  // For client use
    // (end of fields that have to match C4Document_C)

    alloc_slice               _selectedRevID;   // Backing store for _selected.revID
    litecore::CollectionImpl* _collection;      // Owning collection
};


C4_ASSUME_NONNULL_END
