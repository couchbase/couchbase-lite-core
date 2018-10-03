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
#include "RefCounted.hh"
#include "Logging.hh"
#include "function_ref.hh"

namespace litecore {
    class Record;
}

namespace fleece { namespace impl {
    class Dict;
    class Doc;
} }

namespace c4Internal {
    class TreeDocument;
    class VectorDocument;


    /** A versioned LiteCore document.
        This is an abstract base class whose concrete subclasses are TreeDocument (rev-trees) 
        and VectorDocument (version-vectors).
        Note: Its parent 'class' C4Document is the public struct declared in c4Document.h. */
    class Document : public C4Document, C4InstanceCounted {
    public:
        alloc_slice _docIDBuf;
        alloc_slice _revIDBuf;
        alloc_slice _selectedRevIDBuf;
        alloc_slice _loadedBody;

        Document(Database *database)
        :_db(database)
        { }

        Document(const Document&) =default;

        virtual ~Document() { }

        // Returns a new Document object identical to this one (doesn't copy the doc in the db!)
        virtual Document* copy() =0;

        bool mustUseVersioning(C4DocumentVersioning requiredVersioning, C4Error *outError) {
            return external(_db)->mustUseVersioning(requiredVersioning, outError);
        }

        bool mustBeInTransaction(C4Error *outError) {
            return external(_db)->mustBeInTransaction(outError);
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
            selectedRev.revID = revID;
            selectedRev.sequence = sequence;
            selectedRev.flags = currentRevFlagsFromDocFlags(flags);
            selectedRev.body = kC4SliceNull;
            return false;
        }

        virtual bool selectParentRevision() noexcept =0;
        virtual bool selectNextRevision() =0;
        virtual bool selectNextLeafRevision(bool includeDeleted) =0;
        virtual bool selectCommonAncestorRevision(slice revID1, slice revID2) {
            error::_throw(error::UnsupportedOperation);
        }
        virtual alloc_slice remoteAncestorRevID(C4RemoteID) =0;
        virtual void setRemoteAncestorRevID(C4RemoteID) =0;

        virtual bool hasRevisionBody() noexcept =0;
        virtual bool loadSelectedRevBody() =0; // can throw; returns false if compacted away

        virtual alloc_slice detachSelectedRevBody() {
            auto result = _loadedBody;
            if (result.buf)
                _loadedBody = nullslice;
            else
                result = selectedRev.body; // will copy
            selectedRev.body = kC4SliceNull;
            return result;
        }

        Retained<fleece::impl::Doc> fleeceDoc();

        alloc_slice bodyAsJSON(bool canonical =false);

        virtual int32_t putExistingRevision(const C4DocPutRequest&) =0;
        virtual bool putNewRevision(const C4DocPutRequest&) =0;

        virtual void resolveConflict(C4String winningRevID,
                                     C4String losingRevID,
                                     C4Slice mergedBody,
                                     C4RevisionFlags mergedFlags) {
            error::_throw(error::UnsupportedOperation);
        }

        virtual int32_t purgeRevision(C4Slice revID) {
            error::_throw(error::Unimplemented);
        }

        virtual bool removeSelectedRevBody() noexcept {
            return false;
        }

        // Returns false on conflict
        virtual bool save(unsigned maxRevTreeDepth =0) {return true;}

        void requireValidDocID();   // Throws if invalid

        // STATIC UTILITY FUNCTIONS:

        static bool isValidDocID(slice);

        /** Returns true if the given dictionary is a [reference to a] blob. */
        static bool dictIsBlob(const fleece::impl::Dict *dict);

        /** Returns true if the given dictionary is a [reference to a] blob; if so, gets its key. */
        static bool dictIsBlob(const fleece::impl::Dict *dict, blobKey &outKey);

        /** Returns the dict's "digest" property decoded into a blobKey. */
        static bool getBlobKey(const fleece::impl::Dict*, blobKey &outKey);

        using FindBlobCallback = function_ref<bool(const fleece::impl::Dict*)>;
        static bool findBlobReferences(const fleece::impl::Dict*,
                                       const FindBlobCallback&);

        static bool blobIsCompressible(const fleece::impl::Dict *meta);

    protected:
        void clearSelectedRevision() noexcept {
            _selectedRevIDBuf = nullslice;
            selectedRev.revID = {};
            selectedRev.flags = (C4RevisionFlags)0;
            selectedRev.sequence = 0;
            selectedRev.body = kC4SliceNull;
            _loadedBody = nullslice;
        }

        Retained<Database> _db;
    };


    static inline Document *asInternal(C4Document *doc) {
        return (Document*)doc;
    }

} // end namespace
