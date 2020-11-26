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
#include "DataFile.hh"
#include "BlobStore.hh"
#include "RevID.hh"
#include "InstanceCounted.hh"
#include "RefCounted.hh"
#include "Logging.hh"
#include "function_ref.hh"

namespace litecore {
    class Record;
}

namespace fleece { namespace impl {
    class Dict;
    class Doc;
    class Value;
} }

namespace c4Internal {

    /** A versioned LiteCore document.
        This is an abstract base class whose concrete subclasses are TreeDocument (revision trees) 
        and LeafDocument (single revisions)
        Note: Its parent 'class' C4Document is the public struct declared in c4Document.h. */
    class Document : public RefCounted, public C4Document, public fleece::InstanceCountedIn<Document> {
    public:
        alloc_slice const _docIDBuf;
        alloc_slice _revIDBuf;
        alloc_slice _selectedRevIDBuf;

        template <class SLICE>
        Document(Database *database, SLICE docID_)
        :_db(database)
        ,_docIDBuf(std::move(docID_))
        {
            docID = _docIDBuf;
            extraInfo = { };
       }

        Document(const Document&) =default;

        // Returns a new Document object identical to this one (doesn't copy the doc in the db!)
        virtual Document* copy() =0;

#if 0 // unused
        bool mustUseVersioning(C4DocumentVersioning requiredVersioning, C4Error *outError) {
            return _db->mustUseVersioning(requiredVersioning, outError);
        }
#endif
        bool mustBeInTransaction(C4Error *outError) {
            return _db->mustBeInTransaction(outError);
        }

        Database* database()    {return _db;}

        virtual bool exists() =0;
        virtual void loadRevisions() =0;
        virtual bool revisionsLoaded() const noexcept =0;
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

        virtual bool selectCurrentRevision() noexcept {
            // By default just fill in what we know about the current revision:
            if (exists()) {
                selectedRev.revID = revID;
                selectedRev.sequence = sequence;
                selectedRev.flags = currentRevFlagsFromDocFlags(flags);
                selectedRev.body = kC4SliceNull;
            } else {
                clearSelectedRevision();
            }
            return false;
        }

        virtual bool selectParentRevision() noexcept =0;
        virtual bool selectNextRevision() =0;
        virtual bool selectNextLeafRevision(bool includeDeleted) =0;
        virtual bool selectCommonAncestorRevision(slice revID1, slice revID2) {
            failUnsupported();
        }
        virtual alloc_slice remoteAncestorRevID(C4RemoteID) =0;
        virtual void setRemoteAncestorRevID(C4RemoteID) =0;

        virtual bool hasRevisionBody() noexcept =0;
        virtual bool loadSelectedRevBody() =0; // can throw; returns false if compacted away

        virtual alloc_slice detachSelectedRevBody() {
            return alloc_slice(selectedRev.body); // will copy
        }

        virtual Retained<fleece::impl::Doc> fleeceDoc() =0;

        alloc_slice bodyAsJSON(bool canonical =false);

        virtual int32_t putExistingRevision(const C4DocPutRequest&, C4Error*) =0;
        virtual bool putNewRevision(const C4DocPutRequest&) =0;

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

        void setRevID(revid id) {
            if (id.size > 0)
                _revIDBuf = id.expanded();
            else
                _revIDBuf = nullslice;
            revID = _revIDBuf;
        }

        void clearSelectedRevision() noexcept {
            _selectedRevIDBuf = nullslice;
            selectedRev.revID = {};
            selectedRev.flags = (C4RevisionFlags)0;
            selectedRev.sequence = 0;
            selectedRev.body = kC4SliceNull;
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
        DocumentFactory(Database *db)       :_db(db) { }
        Database* database() const          {return _db;}

        virtual ~DocumentFactory() { }
        virtual Retained<Document> newDocumentInstance(C4Slice docID) =0;
        virtual Retained<Document> newDocumentInstance(const Record&) =0;
        virtual Retained<Document> newLeafDocumentInstance(C4Slice docID, C4Slice revID, bool withBody) =0;

        virtual alloc_slice revIDFromVersion(slice version) =0;
        virtual bool isFirstGenRevID(slice revID)               {return false;}

        virtual std::vector<alloc_slice> findAncestors(const std::vector<slice> &docIDs,
                                                       const std::vector<slice> &revIDs,
                                                       unsigned maxAncestors,
                                                       bool mustHaveBodies,
                                                       C4RemoteID remoteDBID) =0;

    private:
        Database* const _db;
    };

} // end namespace
