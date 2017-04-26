//
//  Document.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/18/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once
#include "c4Internal.hh"
#include "c4Document.h"
#include "Database.hh"
#include "DataFile.hh"
#include "BlobStore.hh"
#include "RefCounted.hh"
#include "Logging.hh"
#include "Value.hh"

namespace litecore {
    class Record;
}

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

        Document(Database* database, C4Slice docID)
        :_db(database)
        { }

        Document(Database *database, const Record &doc)
        :_db(database)
        { }

        virtual ~Document() { }

        bool mustUseVersioning(C4DocumentVersioning requiredVersioning, C4Error *outError) {
            return external(_db)->mustUseVersioning(requiredVersioning, outError);
        }

        bool mustBeInTransaction(C4Error *outError) {
            return external(_db)->mustBeInTransaction(outError);
        }

        Database* database()    {return _db;}

        virtual const Record& record() =0;

        virtual bool exists() =0;
        virtual void loadRevisions() =0;
        virtual bool revisionsLoaded() const noexcept =0;
        virtual bool selectRevision(C4Slice revID, bool withBody) =0;   // returns false if not found

        virtual bool selectCurrentRevision() noexcept {
            // By default just fill in what we know about the current revision:
            selectedRev.revID = revID;
            selectedRev.sequence = sequence;
            int revFlags = 0;
            if (flags & kExists) {
                revFlags |= kRevLeaf;
                if (flags & kDeleted)
                    revFlags |= kRevDeleted;
                if (flags & kHasAttachments)
                    revFlags |= kRevHasAttachments;
            }
            selectedRev.flags = (C4RevisionFlags)revFlags;
            selectedRev.body = kC4SliceNull;
            return false;
        }

        virtual bool selectParentRevision() noexcept =0;
        virtual bool selectNextRevision() =0;
        virtual bool selectNextLeafRevision(bool includeDeleted) =0;

        virtual bool hasRevisionBody() noexcept =0;
        virtual bool loadSelectedRevBodyIfAvailable() =0; // can throw; returns false if compacted away

        void loadSelectedRevBody() {
            if (!loadSelectedRevBodyIfAvailable())
                error::_throw(error::Deleted);      // body has been compacted away
        }

        virtual alloc_slice detachSelectedRevBody() {
            auto result = _loadedBody;
            if (result.buf)
                _loadedBody = nullslice;
            else
                result = selectedRev.body; // will copy
            selectedRev.body = kC4SliceNull;
            return result;
        }

        alloc_slice bodyAsJSON() {
            if (!selectedRev.body.buf)
                error::_throw(error::NotFound);
            auto root = fleece::Value::fromTrustedData(selectedRev.body);
            if (!root)
                error::_throw(error::CorruptData);
            return root->toJSON(database()->documentKeys());
        }

        virtual int32_t putExistingRevision(const C4DocPutRequest&) =0;
        virtual bool putNewRevision(const C4DocPutRequest&) =0;

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

        /** Returns true if this is the name of a 1.x metadata property ("_id", "_rev", etc.) */
        static bool isOldMetaProperty(slice key);

        /** Returns true if the document contains 1.x metadata properties (at top level). */
        static bool hasOldMetaProperties(const fleece::Dict*);

        /** Re-encodes to Fleece, without any 1.x metadata properties. */
        static alloc_slice encodeStrippingOldMetaProperties(const fleece::Dict*);

        /** Returns true if the given dictionary is a [reference to a] blob; if so, gets its key. */
        static bool dictIsBlob(const fleece::Dict *dict, blobKey &outKey);

        using FindBlobCallback = function<void(const blobKey &key, uint64_t size)>;
        static void findBlobReferences(const fleece::Dict*, const FindBlobCallback&);

    protected:
        void clearSelectedRevision() noexcept {
            _selectedRevIDBuf = nullslice;
            selectedRev.revID = kC4SliceNull;
            selectedRev.flags = (C4RevisionFlags)0;
            selectedRev.sequence = 0;
            selectedRev.body = kC4SliceNull;
            _loadedBody = nullslice;
        }

        static void findBlobReferences(const fleece::Value *val, const FindBlobCallback &callback);

        Retained<Database> _db;
    };


    static inline Document *internal(C4Document *doc) {
        return (Document*)doc;
    }

} // end namespace
