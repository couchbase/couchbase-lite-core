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
#include "VersionedDocument.hh"

namespace cbforest {


class C4DocumentInternal : public C4Document, c4Internal::InstanceCounted {
public:
    alloc_slice _revIDBuf;
    alloc_slice _selectedRevIDBuf;
    alloc_slice _loadedBody;

    static C4DocumentInternal* newInstance(C4Database* database, C4Slice docID);
    static C4DocumentInternal* newInstance(C4Database* database, Document &&doc);

    virtual ~C4DocumentInternal() {
        _db->release();
    }

    C4DocumentInternal(C4Database* database, C4Slice docID)
    :_db(database->retain())
    { }

    C4DocumentInternal(C4Database *database, Document &&doc)
    :_db(database->retain())
    { }

    C4Database* database()    {return _db;}

    bool mustBeInTransaction(C4Error *outError) {
        return _db->mustBeInTransaction(outError);
    }

    virtual C4SliceResult type() =0;
    virtual void setType(C4Slice) =0;

    virtual bool exists() =0;
    virtual bool loadRevisions(C4Error *outError) =0;
    virtual bool revisionsLoaded() const =0;
    virtual bool selectRevision(C4Slice revID, bool withBody, C4Error *outError) = 0;
    virtual bool selectRevision(const Revision *rev, C4Error *outError =NULL) =0;
    virtual bool selectCurrentRevision() =0;
    virtual bool selectParentRevision() =0;
    virtual bool selectNextRevision() =0;
    virtual bool selectNextLeafRevision(bool includeDeleted, bool withBody, C4Error *outError) =0;

    virtual bool hasRevisionBody() =0;
    virtual bool loadSelectedRevBody(C4Error *outError) =0;

    virtual void save(unsigned maxRevTreeDepth) =0;
    virtual int32_t purgeRevision(C4Slice revID) =0;


protected:
    C4Database* const _db;
};


static inline C4DocumentInternal *internal(C4Document *doc) {
    return (C4DocumentInternal*)doc;
}


    class C4DocumentV2;


} // end namespace

#endif /* c4DocInternal_h */
