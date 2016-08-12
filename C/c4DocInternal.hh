//
//  c4DocInternal.hh
//  CBForest
//
//  Created by Jens Alfke on 7/18/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef c4DocInternal_h
#define c4DocInternal_h
#include "Database.hh"
#include "Document.hh"
#include "LogInternal.hh"

namespace c4Internal {


class C4DocumentV1;
class C4DocumentV2;


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

    bool mustBeSchema(int schema, C4Error *outError) {
        return _db->mustBeSchema(schema, outError);
    }

    bool mustBeInTransaction(C4Error *outError) {
        return _db->mustBeInTransaction(outError);
    }

    C4Database* database()    {return _db;}

    virtual const Document& document() =0;

    virtual slice type() =0;    // should not throw
    virtual void setType(slice) =0;    // should not throw

    virtual bool exists() =0;
    virtual void loadRevisions() =0;
    virtual bool revisionsLoaded() const =0;
    virtual void selectRevision(C4Slice revID, bool withBody) =0;

    virtual bool selectCurrentRevision() {    // should not throw
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

    virtual bool selectParentRevision() =0;     // should not throw
    virtual bool selectNextRevision() =0;
    virtual bool selectNextLeafRevision(bool includeDeleted, bool withBody) =0;

    virtual bool hasRevisionBody() =0;
    virtual bool loadSelectedRevBodyIfAvailable() =0; // can throw; returns false if compacted away

    void loadSelectedRevBody() {
        if (!loadSelectedRevBodyIfAvailable())
            error::_throw(error::Deleted);      // body has been compacted away
    }

    virtual int32_t putExistingRevision(const C4DocPutRequest&) =0;
    virtual bool putNewRevision(const C4DocPutRequest&) =0;

    virtual int32_t purgeRevision(C4Slice revID) {
        error::_throw(error::Unimplemented);
    }

protected:
    static C4DocumentInternal* newV2Instance(C4Database* database, C4Slice docID);
    static C4DocumentInternal* newV2Instance(C4Database* database, const Document&);

    void clearSelectedRevision() {
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

#endif /* c4DocInternal_h */
