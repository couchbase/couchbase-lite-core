//
//  RevisionStore.cc
//  CBForest
//
//  Created by Jens Alfke on 7/8/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "RevisionStore.hh"
#include "Revision.hh"
#include "DocEnumerator.hh"

namespace cbforest {


    RevisionStore::RevisionStore(Database *db, bool usingCAS)
    :_db(db),
     _nonCurrent(db->getKeyStore("revs")),
     _usingCAS(usingCAS)
    { }

    Revision* RevisionStore::get(slice docID, KeyStore::contentOptions opt) const {
        auto rev = new Revision(docID, slice::null, *_db, opt);
        if (!rev->document().exists())
            return nullptr;
        return rev;
    }

    Revision* RevisionStore::get(slice docID, slice revID, KeyStore::contentOptions opt) const {
        // No revID means current revision:
        if (revID.size == 0)
            return get(docID, opt);

        // Look in non-current revision store:
        std::unique_ptr<Revision> rev { getNonCurrent(docID, revID, opt) };
        if (rev)
            return rev.release();

        // Not found; see if it's the current revision:
        rev.reset( get(docID, opt) );
        if (rev && rev->revID() == revID)
            return rev.release();
        return nullptr;
    }

    Revision* RevisionStore::getNonCurrent(slice docID, slice revID,
                                           KeyStore::contentOptions opt) const
    {
        CBFAssert(revID.size > 0);
        std::unique_ptr<Revision> rev {new Revision(docID, revID, _nonCurrent, opt)};
        if (rev->document().exists())
            return rev.release();
        return nullptr;
    }


    versionVector::order RevisionStore::insert(Revision &newRev, Transaction &t) {
        std::unique_ptr<Revision> current { get(newRev.docID(), KeyStore::kMetaOnly) };
        auto cmp = current ? newRev.version().compareTo(current->version()) : versionVector::kNewer;
        switch (cmp) {
            case versionVector::kSame:
            case versionVector::kOlder:
                // This revision already exists, or is obsolete: no-op
                break;

            case versionVector::kNewer:
                // This revision is newer than the current one, so replace it:
                backupCASVersion(newRev, current.get(), t);
                if (current && (current->isConflicted()
                                    || shouldDeleteCASBackup(newRev, current.get()))) {
                    deleteAncestors(newRev, t);
                }
                newRev.setCurrent(true);    // update revID to just docID
                t.write(newRev.document());
                break;

            case versionVector::kConflicting:
                // Oops, they conflict:
                CBFAssert(false);//TODO
                break;
        }
        return cmp;
    }


#pragma mark - CAS SERVER STUFF:


    // If a revision from the CAS server is being replaced by a newer revision that isn't,
    // back it up to the nonCurrent store so we can merge with it if necessary.
    void RevisionStore::backupCASVersion(const Revision &newRev, Revision *current, Transaction &t)
    {
        if (_usingCAS && current && current->isFromCASServer() && !newRev.isFromCASServer()) {
            _db->readBody(current->document());     // load the body
            current->setCurrent(false);             // append the revID to the key
            t(_nonCurrent).write(current->document());
        }
    }

    // Is the current CAS-server backup revision (if any) now obsolete?
    // This happens when a revision from the CAS server replaces one that isn't.
    bool RevisionStore::shouldDeleteCASBackup(const Revision &newRev, const Revision *current) {
        return _usingCAS && current && newRev.isFromCASServer() && !current->isFromCASServer();
    }

    // Is `rev` a saved CAS-server backup of the current revision `child`?
    bool RevisionStore::isSavedCASBackup(const Revision &rev, const Revision &child) {
        if (!_usingCAS)
            return false;
        auto cas = rev.version().current().CAS();
        return cas > 0 && cas == child.version().CAS();
    }


#pragma mark - ENUMERATION:


    static const DocEnumerator::Options kRevEnumOptions = {
        0,
        UINT_MAX,
        false,
        false,  // no inclusiveStart
        false,  // no inclusiveEnd
        false,
        KeyStore::kMetaOnly
    };


    void RevisionStore::deleteAncestors(Revision &child, Transaction &t) {
        DocEnumerator e(_nonCurrent,
                        Revision::startKeyForDocID(child.docID()),
                        Revision::endKeyForDocID(child.docID()),
                        kRevEnumOptions);
        while (e.next()) {
            Revision rev(e.moveDoc());
            if (rev.version().compareTo(child.version()) == versionVector::kOlder
                    && !isSavedCASBackup(rev, child)) {
                t.del(rev.document());
            }
        }
    }

}