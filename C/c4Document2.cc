//
//  c4Document2.cc
//  CBForest
//
//  Created by Jens Alfke on 7/18/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "c4Impl.hh"
#include "c4Document.h"
#include "c4Database.h"
#include "c4Private.h"

#include "C4DocInternal.hh"
#include "CASRevisionStore.hh"


namespace cbforest {

    class C4DocumentV2 : public C4DocumentInternal {
    public:
        C4DocumentV2(C4Database* database, C4Slice docID)
        :C4DocumentInternal(database, docID)
        { }

        C4DocumentV2(C4Database *database, Document &&doc)
        :C4DocumentInternal(database, std::move(doc))
        { }

        ~C4DocumentV2() { }

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
        
    };

}



CASRevisionStore& c4Database::revisionStore() {
    if (!_revisionStore)
        _revisionStore = new CASRevisionStore(this);
    return *_revisionStore;
}
