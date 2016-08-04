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


class C4DocumentV1;
class C4DocumentV2;


class C4DocumentInternal : public C4Document, c4Internal::InstanceCounted {
public:
    alloc_slice _revIDBuf;
    alloc_slice _selectedRevIDBuf;
    alloc_slice _loadedBody;

    static C4DocumentInternal* newInstance(C4Database* database, C4Slice docID);
    static C4DocumentInternal* newInstance(C4Database* database, const Document &doc);

    virtual ~C4DocumentInternal() {
        _db->release();
    }

    C4DocumentInternal(C4Database* database, C4Slice docID)
    :_db(database->retain())
    { }

    C4DocumentInternal(C4Database *database, const Document &doc)
    :_db(database->retain())
    { }

    bool mustBeSchema(int schema, C4Error *outError);

    C4Database* database()    {return _db;}

    bool mustBeInTransaction(C4Error *outError) {
        return _db->mustBeInTransaction(outError);
    }

    virtual const Document& document() =0;

    virtual slice type() =0;    // should not throw
    virtual void setType(slice) =0;    // should not throw

    virtual bool exists() =0;
    virtual void loadRevisions() =0;
    virtual bool revisionsLoaded() const =0;
    virtual void selectRevision(C4Slice revID, bool withBody) = 0;
    virtual bool selectCurrentRevision() =0;    // should not throw
    virtual bool selectParentRevision() =0;     // should not throw
    virtual bool selectNextRevision() =0;
    virtual bool selectNextLeafRevision(bool includeDeleted, bool withBody) =0;

    virtual bool hasRevisionBody() =0;
    virtual bool loadSelectedRevBodyIfAvailable() =0; // can throw; returns false if compacted away

    void loadSelectedRevBody() {
        if (!loadSelectedRevBodyIfAvailable())
            error::_throw(error::Deleted);      // body has been compacted away
    }

    virtual void save(unsigned maxRevTreeDepth) =0;
    virtual int32_t purgeRevision(C4Slice revID) =0;


protected:
    static C4DocumentInternal* newV2Instance(C4Database* database, C4Slice docID);
    static C4DocumentInternal* newV2Instance(C4Database* database, const Document &doc);

    C4Database* const _db;
};


static inline C4DocumentInternal *internal(C4Document *doc) {
    return (C4DocumentInternal*)doc;
}


} // end namespace

#endif /* c4DocInternal_h */
