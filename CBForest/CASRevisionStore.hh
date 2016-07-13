//
//  CASRevisionStore.hh
//  CBForest
//
//  Created by Jens Alfke on 7/11/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef CASRevisionStore_hh
#define CASRevisionStore_hh
#include "RevisionStore.hh"

namespace cbforest {


    /** RevisionStore that also supports revisions coming from a server that only tags documents
        with an integer clock (a "CAS" value) and doesn't support conflicts or merging. */
    class CASRevisionStore : public RevisionStore {
    public:

        CASRevisionStore(Database *db)              :RevisionStore(db) { }

        /** Returns the latest known revision from the CAS server. */
        Revision::Ref getLatestCASServerRevision(slice docID);

        /** Returns the base revision from the CAS server, the one the current rev is based on. */
        Revision::Ref getBaseCASServerRevision(Revision&);

        /** Inserts a new revision from the CAS server. */
        Revision::Ref insertCAS(slice docID,
                               generation cas,
                               Revision::BodyParams,
                               Transaction&);

        /** Assigns a revision a new CAS value after it's pushed to the CAS server. */
        void assignCAS(Revision&, generation cas, Transaction &t);

    private:
        virtual void backupCASVersion(Revision &curRev, const Revision &incomingRev, Transaction &t);
        virtual bool shouldDeleteCASBackup(const Revision &newRev, const Revision *current);
        virtual bool shouldKeepAncestor(const Revision &rev, const Revision &child);
        
        Revision::Ref writeCASRevision(const Revision *parent,
                                       bool current,
                                       slice docID, generation cas,
                                       Revision::BodyParams body,
                                       Transaction &t);
    };

}
#endif /* CASRevisionStore_hh */
