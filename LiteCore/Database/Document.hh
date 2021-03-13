//
// Document.hh
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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
#include "c4Internal.hh"
#include "c4Document.h"
#include "DatabaseImpl.hh"
#include "BlobStore.hh"
#include "InstanceCounted.hh"
#include "RefCounted.hh"
#include "function_ref.hh"
#include <vector>

namespace litecore {
    class Record;
    class revid;
}

namespace fleece { namespace impl {
    class Dict;
    class Doc;
    class Value;
} }

namespace c4Internal {

    /** A LiteCore document.
        This is an abstract base class whose concrete subclasses are TreeDocument (revision trees) 
        and VectorDocument (version vectors).
        Note: Its parent 'class' C4Document is the public struct declared in c4Document.h. */
    class Document : public RefCounted, public C4Document, public fleece::InstanceCountedIn<Document> {
    public:

        // Static utility functions:

        static char* generateID(char *outDocID, size_t bufferSize) noexcept;

        static bool equalRevIDs(slice revID1, slice revID2) noexcept;
        static unsigned getRevIDGeneration(slice revID) noexcept;

        static C4RevisionFlags currentRevFlagsFromDocFlags(C4DocumentFlags docFlags) noexcept {
            C4RevisionFlags revFlags = 0;
            if (docFlags & kDocExists) {
                revFlags |= kRevLeaf;
                // For stupid historical reasons C4DocumentFlags and C4RevisionFlags aren't compatible
                if (docFlags & kDocDeleted)
                    revFlags |= kRevDeleted;
                if (docFlags & kDocHasAttachments)
                    revFlags |= kRevHasAttachments;
                if (docFlags & (C4DocumentFlags)DocumentFlags::kSynced)
                    revFlags |= kRevKeepBody;
            }
            return revFlags;
        }

        /** Returns the Document instance, if any, that contains the given Fleece value. */
        static Document* containing(FLValue) noexcept;

        static bool isOldMetaProperty(slice propertyName) noexcept;
        static bool hasOldMetaProperties(FLDict) noexcept;

        static bool isValidDocID(slice) noexcept;

        static alloc_slice encodeStrippingOldMetaProperties(FLDict properties, FLSharedKeys);

        // Selecting revisions:

        virtual bool selectCurrentRevision() noexcept {
            // By default just fill in what we know about the current revision:
            if (exists()) {
                _selectedRevIDBuf = _revIDBuf;
                selectedRev.revID = revID;
                selectedRev.sequence = sequence;
                selectedRev.flags = currentRevFlagsFromDocFlags(flags);
            } else {
                clearSelectedRevision();
            }
            return false;
        }

        virtual bool selectRevision(C4Slice revID, bool withBody) =0;   // returns false if not found

        virtual bool selectParentRevision() noexcept    {failUnsupported();}
        virtual bool selectNextRevision() =0;
        virtual bool selectNextLeafRevision(bool includeDeleted) =0;
        virtual bool selectCommonAncestorRevision(slice revID1, slice revID2) {
            failUnsupported();
        }

        // Revision info:

        virtual bool loadSelectedRevBody() =0; // can throw; returns false if compacted away

        virtual bool hasRevisionBody() noexcept =0;

        virtual slice getSelectedRevBody() noexcept =0;

        alloc_slice bodyAsJSON(bool canonical =false);

        virtual FLDict getSelectedRevRoot() noexcept {
            if (slice body = getSelectedRevBody(); body)
            return FLValue_AsDict(FLValue_FromData(body, kFLTrusted));
            else
                return nullptr;
        }

        virtual alloc_slice getSelectedRevIDGlobalForm() {
            DebugAssert(_selectedRevIDBuf == slice(selectedRev.revID));
            return _selectedRevIDBuf;
        }

        virtual alloc_slice getSelectedRevHistory(unsigned maxHistory,
                                                  const C4String backToRevs[],
                                                  unsigned backToRevsCount) {failUnsupported();}

        // Remote database revision tracking:

        virtual alloc_slice remoteAncestorRevID(C4RemoteID) =0;
        virtual void setRemoteAncestorRevID(C4RemoteID, C4String revID) =0;

        // Purging:

        virtual bool removeSelectedRevBody() noexcept {
            return false;
        }

        virtual int32_t purgeRevision(C4Slice revID) {
            failUnsupported();
        }

        // Conflicts:

        void resolveConflict(C4String winningRevID,
                             C4String losingRevID,
                             FLDict mergedProperties,
                             C4RevisionFlags mergedFlags,
                             bool pruneLosingBranch =true);


        virtual void resolveConflict(C4String winningRevID,
                                     C4String losingRevID,
                                     C4Slice mergedBody,
                                     C4RevisionFlags mergedFlags,
                                     bool pruneLosingBranch =true)
        {
            failUnsupported();
        }


        // Updating & Saving:

        /// Returns updated document (may be same instance), or nullptr on conflict.
        Retained<Document> update(slice revBody, C4RevisionFlags);

        // Returns false on conflict
        virtual bool save(unsigned maxRevTreeDepth =0) =0;


#pragma mark - INTERNALS:

//TEMP    protected:
        alloc_slice const _docIDBuf;    // Backing store for C4Document::docID
        alloc_slice _revIDBuf;          // Backing store for C4Document::revID
        alloc_slice _selectedRevIDBuf;  // Backing store for C4Document::selectedRevision::revID

        template <class SLICE>
        Document(DatabaseImpl *database, SLICE docID_)
        :_db(database)
        ,_docIDBuf(move(docID_))
        {
            docID = _docIDBuf;
            extraInfo = { };
       }

        Document(const Document&) =default;

        /** Returns the contents of a blob referenced by a dict. Inline data will be decoded if
             necessary, or the "digest" property will be looked up in the BlobStore if one is
             provided.
             Returns a null slice if the blob data is not inline but no BlobStore is given.
             Otherwise throws an exception if it's unable to return data. */
        static fleece::alloc_slice getBlobData(FLDict NONNULL, C4BlobStore*);

        using FindBlobCallback = function_ref<bool(FLDict)>;

        static bool findBlobReferences(FLDict, const FindBlobCallback&);

        bool mustBeInTransaction(C4Error *err) noexcept {return _db->mustBeInTransaction(err);}
        void mustBeInTransaction()                      {_db->mustBeInTransaction();}

        DatabaseImpl* database()                                            {return _db;}
        const DatabaseImpl* database() const                                {return _db;}

        virtual bool exists() =0;
        virtual bool loadRevisions() MUST_USE_RESULT =0;
        virtual bool revisionsLoaded() const noexcept =0;

        static C4DocumentFlags docFlagsFromCurrentRevFlags(C4RevisionFlags revFlags) {
            C4DocumentFlags docFlags = kDocExists;
            if (revFlags & kRevDeleted)         docFlags |= kDocDeleted;
            if (revFlags & kRevHasAttachments)  docFlags |= kDocHasAttachments;
            if (revFlags & kRevIsConflict)      docFlags |= kDocConflicted;
            return docFlags;
        }

        virtual alloc_slice detachSelectedRevBody() {
            return alloc_slice(getSelectedRevBody()); // will copy
        }

        // Returns the index (in rq.history) of the common ancestor; or -1 on error
        virtual int32_t putExistingRevision(const C4DocPutRequest&, C4Error*) =0;

        // Returns false on error
        virtual bool putNewRevision(const C4DocPutRequest&, C4Error*) =0;

        void requireValidDocID();   // Throws if invalid

    protected:
        virtual ~Document() {
            destructExtraInfo(extraInfo);
        }

        void setRevID(revid);

        void clearSelectedRevision() noexcept {
            _selectedRevIDBuf = nullslice;
            selectedRev.revID = {};
            selectedRev.flags = (C4RevisionFlags)0;
            selectedRev.sequence = 0;
        }

        [[noreturn]] void failUnsupported() {
            error::_throw(error::UnsupportedOperation);
        }

        Retained<DatabaseImpl> _db;
    };


    static inline Document *asInternal(C4Document *doc) {
        return (Document*)doc;
    }


    /** Abstract interface for creating Document instances; owned by a Database. */
    class DocumentFactory {
    public:
        DocumentFactory(DatabaseImpl *db)                       :_db(db) { }
        virtual ~DocumentFactory()                          { }
        DatabaseImpl* database() const                          {return _db;}

        virtual bool isFirstGenRevID(slice revID) const     {return false;}

        virtual Retained<Document> newDocumentInstance(C4Slice docID, ContentOption) =0;
        virtual Retained<Document> newDocumentInstance(const Record&) =0;

        virtual std::vector<alloc_slice> findAncestors(const std::vector<slice> &docIDs,
                                                       const std::vector<slice> &revIDs,
                                                       unsigned maxAncestors,
                                                       bool mustHaveBodies,
                                                       C4RemoteID remoteDBID) =0;
    private:
        DatabaseImpl* const _db;    // Unretained, to avoid ref-cycle with Database
    };


    // for disambiguation with C4Document
    static inline Document* retain(Document *doc)     {retain((RefCounted*)doc); return doc;}
    static inline void release(Document *doc)         {release((RefCounted*)doc);}

} // end namespace
