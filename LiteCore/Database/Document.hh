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
#include "Database.hh"
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
        alloc_slice const _docIDBuf;    // Backing store for C4Document::docID
        alloc_slice _revIDBuf;          // Backing store for C4Document::revID
        alloc_slice _selectedRevIDBuf;  // Backing store for C4Document::selectedRevision::revID

        template <class SLICE>
        Document(Database *database, SLICE docID_)
        :_db(database)
        ,_docIDBuf(move(docID_))
        {
            docID = _docIDBuf;
            extraInfo = { };
       }

        Document(const Document&) =default;

        bool mustBeInTransaction(C4Error *outError) {
            return _db->mustBeInTransaction(outError);
        }

        Database* database()                                            {return _db;}
        const Database* database() const                                {return _db;}

        virtual bool exists() =0;
        virtual void loadRevisions()                                    { }
        virtual bool revisionsLoaded() const noexcept                   {return true;}
        virtual bool selectRevision(C4Slice revID, bool withBody) =0;   // returns false if not found

        static C4RevisionFlags currentRevFlagsFromDocFlags(C4DocumentFlags docFlags) {
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

        static C4DocumentFlags docFlagsFromCurrentRevFlags(C4RevisionFlags revFlags) {
            C4DocumentFlags docFlags = kDocExists;
            if (revFlags & kRevDeleted)         docFlags |= kDocDeleted;
            if (revFlags & kRevHasAttachments)  docFlags |= kDocHasAttachments;
            if (revFlags & kRevIsConflict)      docFlags |= kDocConflicted;
            return docFlags;
        }

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

        virtual bool selectParentRevision() noexcept    {failUnsupported();}
        virtual bool selectNextRevision() =0;
        virtual bool selectNextLeafRevision(bool includeDeleted) =0;
        virtual bool selectCommonAncestorRevision(slice revID1, slice revID2) {
            failUnsupported();
        }
        virtual alloc_slice remoteAncestorRevID(C4RemoteID) =0;
        virtual void setRemoteAncestorRevID(C4RemoteID, C4String revID) =0;

        virtual bool hasRevisionBody() noexcept =0;
        virtual bool loadSelectedRevBody() =0; // can throw; returns false if compacted away
        virtual slice getSelectedRevBody() noexcept =0;
        virtual alloc_slice detachSelectedRevBody() {
            return alloc_slice(getSelectedRevBody()); // will copy
        }

        virtual FLDict getSelectedRevRoot() noexcept {
            if (slice body = getSelectedRevBody(); body)
                return FLValue_AsDict(FLValue_FromData(body, kFLTrusted));
            else
                return nullptr;
        }

        virtual alloc_slice getSelectedRevHistory(unsigned maxHistory,
                                                  const C4String backToRevs[],
                                                  unsigned backToRevsCount) {failUnsupported();}

        virtual alloc_slice getSelectedRevIDGlobalForm() {
            DebugAssert(_selectedRevIDBuf == slice(selectedRev.revID));
            return _selectedRevIDBuf;
        }

        alloc_slice bodyAsJSON(bool canonical =false);

        // Returns the index (in rq.history) of the common ancestor; or -1 on error
        virtual int32_t putExistingRevision(const C4DocPutRequest&, C4Error*) =0;

        // Returns false on error
        virtual bool putNewRevision(const C4DocPutRequest&, C4Error*) =0;

        virtual void resolveConflict(C4String winningRevID,
                                     C4String losingRevID,
                                     C4Slice mergedBody,
                                     C4RevisionFlags mergedFlags,
                                     bool pruneLosingBranch =true)
        {
            failUnsupported();
        }

        virtual int32_t purgeRevision(C4Slice revID) {
            failUnsupported();
        }

        virtual bool removeSelectedRevBody() noexcept {
            return false;
        }

        // Returns false on conflict
        virtual bool save(unsigned maxRevTreeDepth =0) =0;

        void requireValidDocID();   // Throws if invalid

        // STATIC UTILITY FUNCTIONS:

        static bool isValidDocID(slice);

        /** Returns the Document instance, if any, that contains the given Fleece value. */
        static C4Document* containing(const fleece::impl::Value*);

        /** Returns true if the given dictionary is a [reference to a] blob. */
        static bool dictIsBlob(const fleece::impl::Dict *dict);

        /** Returns true if the given dictionary is a [reference to a] blob; if so, gets its key. */
        static bool dictIsBlob(const fleece::impl::Dict *dict, blobKey &outKey);

        /** Returns the contents of a blob referenced by a dict. Inline data will be decoded if
            necessary, or the "digest" property will be looked up in the BlobStore if one is
            provided.
            Returns a null slice if the blob data is not inline but no BlobStore is given.
            Otherwise throws an exception if it's unable to return data. */
        static fleece::alloc_slice getBlobData(const fleece::impl::Dict *dict NONNULL,
                                               BlobStore*);

        /** Returns the dict's "digest" property decoded into a blobKey. */
        static bool getBlobKey(const fleece::impl::Dict*, blobKey &outKey);

        using FindBlobCallback = function_ref<bool(const fleece::impl::Dict*)>;
        static bool findBlobReferences(const fleece::impl::Dict*,
                                       const FindBlobCallback&);

        static bool blobIsCompressible(const fleece::impl::Dict *meta);

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

        Retained<Database> _db;
    };


    static inline Document *asInternal(C4Document *doc) {
        return (Document*)doc;
    }


    /** Abstract interface for creating Document instances; owned by a Database. */
    class DocumentFactory {
    public:
        DocumentFactory(Database *db)                       :_db(db) { }
        virtual ~DocumentFactory()                          { }
        Database* database() const                          {return _db;}

        virtual bool isFirstGenRevID(slice revID) const     {return false;}

        virtual Retained<Document> newDocumentInstance(C4Slice docID, ContentOption) =0;
        virtual Retained<Document> newDocumentInstance(const Record&) =0;

        virtual std::vector<alloc_slice> findAncestors(const std::vector<slice> &docIDs,
                                                       const std::vector<slice> &revIDs,
                                                       unsigned maxAncestors,
                                                       bool mustHaveBodies,
                                                       C4RemoteID remoteDBID) =0;
    private:
        Database* const _db;    // Unretained, to avoid ref-cycle with Database
    };

} // end namespace
