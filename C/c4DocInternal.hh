//
//  c4DocInternal.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/18/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once
#include "DataFile.hh"
#include "Document.hh"
#include "LogInternal.hh"

namespace c4Internal {


class C4TreeDocument;
class C4VectorDocument;


class C4DocumentInternal : public C4Document, InstanceCounted {
public:
    alloc_slice _revIDBuf;
    alloc_slice _selectedRevIDBuf;
    alloc_slice _loadedBody;

    C4DocumentInternal(C4Database* database, C4Slice docID)
    :_db(database)
    { }

    C4DocumentInternal(C4Database *database, const Document &doc)
    :_db(database)
    { }

    virtual ~C4DocumentInternal() { }

    bool mustUseVersioning(C4DocumentVersioning requiredVersioning, C4Error *outError) {
        return _db->mustUseVersioning(requiredVersioning, outError);
    }

    bool mustBeInTransaction(C4Error *outError) {
        return _db->mustBeInTransaction(outError);
    }

    C4Database* database()    {return _db;}

    virtual const Document& document() =0;

    virtual slice type() noexcept =0;    // should not throw
    virtual void setType(slice) noexcept =0;    // should not throw

    virtual bool exists() =0;
    virtual void loadRevisions() =0;
    virtual bool revisionsLoaded() const noexcept =0;
    virtual bool selectRevision(C4Slice revID, bool withBody) =0;   // returns false if not found

    virtual bool selectCurrentRevision() noexcept {    // should not throw
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
        selectedRev.body = slice::null;
        return false;
    }

    virtual bool selectParentRevision() noexcept =0;     // should not throw
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
            _loadedBody = slice::null;
        else
            result = selectedRev.body; // will copy
        selectedRev.body = slice::null;
        return result;
    }



    virtual int32_t putExistingRevision(const C4DocPutRequest&) =0;
    virtual bool putNewRevision(const C4DocPutRequest&) =0;

    virtual int32_t purgeRevision(C4Slice revID) {
        error::_throw(error::Unimplemented);
    }

protected:
    static C4DocumentInternal* newV2Instance(C4Database* database, C4Slice docID);
    static C4DocumentInternal* newV2Instance(C4Database* database, const Document&);

    void clearSelectedRevision() noexcept {
        _selectedRevIDBuf = slice::null;
        selectedRev.revID = slice::null;
        selectedRev.flags = (C4RevisionFlags)0;
        selectedRev.sequence = 0;
        selectedRev.body = slice::null;
        _loadedBody = slice::null;
    }

    Retained<C4Database> _db;
};


static inline C4DocumentInternal *internal(C4Document *doc) {
    return (C4DocumentInternal*)doc;
}


} // end namespace
