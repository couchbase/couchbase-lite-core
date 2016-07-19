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
#include "Revision.hh"


namespace cbforest {

    class C4DocumentV2 : public C4DocumentInternal {
    public:
        C4DocumentV2(C4Database* database, C4Slice docID)
        :C4DocumentInternal(database, docID),
         _store(database->revisionStore()),
         _current(_store.get(docID))
        {
        }

        C4DocumentV2(C4Database *database, Document &&doc)
        :C4DocumentInternal(database, std::move(doc)),
         _store(database->revisionStore())
        { }

        ~C4DocumentV2() { }

        virtual const Document& document() override {
            return _current->document();
        }

        virtual slice type() override               {return _current->docType();}
        virtual void setType(slice type) override   {/*TEMP _current->setType(type);*/}

        virtual bool exists() override              {return _current->exists();}
        virtual void loadRevisions() override;
        virtual bool revisionsLoaded() const override;
        virtual void selectRevision(C4Slice revID, bool withBody) override;
        virtual bool selectCurrentRevision() override;
        virtual bool selectParentRevision() override;
        virtual bool selectNextRevision() override;
        virtual bool selectNextLeafRevision(bool includeDeleted, bool withBody) override;

        virtual bool hasRevisionBody() override;
        virtual bool loadSelectedRevBodyIfAvailable() override;

        virtual void save(unsigned maxRevTreeDepth) override;
        virtual int32_t purgeRevision(C4Slice revID) override;

    private:
        CASRevisionStore &_store;
        Revision::Ref _current;
    };


    C4DocumentInternal* C4DocumentInternal::newV2Instance(C4Database* database, C4Slice docID) {
        return nullptr;// new C4DocumentV2(database, docID);
    }

    C4DocumentInternal* C4DocumentInternal::newV2Instance(C4Database* database, Document &&doc) {
        return nullptr;//new C4DocumentV2(database, std::move(doc));
    }

}



CASRevisionStore& c4Database::revisionStore() {
    if (!_revisionStore)
        _revisionStore.reset(new CASRevisionStore(this));
    return *_revisionStore;
}
