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
#include "RefCounted.hh"
#include "Logging.hh"

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
    class Document : public C4Document, InstanceCounted {
    public:
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

        virtual slice type() noexcept =0;
        virtual void setType(slice) noexcept =0;

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
            selectedRev.body = nullslice;
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
            selectedRev.body = nullslice;
            return result;
        }

        virtual int32_t putExistingRevision(const C4DocPutRequest&) =0;
        virtual bool putNewRevision(const C4DocPutRequest&) =0;

        virtual int32_t purgeRevision(C4Slice revID) {
            error::_throw(error::Unimplemented);
        }

    protected:
        void clearSelectedRevision() noexcept {
            _selectedRevIDBuf = nullslice;
            selectedRev.revID = nullslice;
            selectedRev.flags = (C4RevisionFlags)0;
            selectedRev.sequence = 0;
            selectedRev.body = nullslice;
            _loadedBody = nullslice;
        }

        Retained<Database> _db;
    };


    static inline Document *internal(C4Document *doc) {
        return (Document*)doc;
    }

} // end namespace
