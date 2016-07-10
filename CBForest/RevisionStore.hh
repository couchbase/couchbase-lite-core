//
//  RevisionStore.hh
//  CBForest
//
//  Created by Jens Alfke on 7/8/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef RevisionStore_hh
#define RevisionStore_hh
#include "Database.hh"
#include "VersionVector.hh"

namespace cbforest {

    class Revision;


    /** Manages storage of version-vectored document revisions in a Database. */
    class RevisionStore {
    public:

        RevisionStore(Database*, bool usingCAS);

        Revision* get(slice docID,
                      KeyStore::contentOptions = KeyStore::kDefaultContent) const;
        Revision* get(slice docID, slice revID,
                      KeyStore::contentOptions = KeyStore::kDefaultContent) const;

        versionVector::order insert(Revision&, Transaction&);

    private:
        Revision* getNonCurrent(slice docID, slice revID, KeyStore::contentOptions) const;
        void backupCASVersion(const Revision &rev, Revision *current, Transaction &t);
        bool shouldDeleteCASBackup(const Revision &newRev, const Revision *current);
        bool isSavedCASBackup(const Revision &rev, const Revision &child);
        void deleteAncestors(Revision&, Transaction&);

        Database *_db;
        KeyStore &_nonCurrent;
        const bool _usingCAS;
    };

}

#endif /* RevisionStore_hh */
