//
//  CASRevisionStore.cc
//  CBForest
//
//  Created by Jens Alfke on 7/11/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "CASRevisionStore.hh"
#include "Revision.hh"
#include "DocEnumerator.hh"

namespace cbforest {


    Revision::Ref CASRevisionStore::getLatestCASServerRevision(slice docID) {
        Revision::Ref cur = get(docID);
        if (cur && !cur->isFromCASServer())
            cur = nullptr;
        auto e = enumerateRevisions(docID, kCASServerPeerID);
        while (e.next()) {
            Revision rev(e.moveDoc());
            if (rev.isFromCASServer())
                if (!cur || rev.CAS() > cur->CAS())
                    cur.reset(new Revision(std::move(rev)));
        }
        return cur;
        //OPT: Wouldn't have to compare revs if the gen numbers were ordered in the keys.
    }


    Revision::Ref CASRevisionStore::getBaseCASServerRevision(Revision &current) {
        generation parentCAS = current.version().CASBase();
        if (parentCAS == 0)
            return nullptr;
        return getNonCurrent(current.docID(),
                             version(parentCAS, kCASServerPeerID).asString(),
                             KeyStore::kDefaultContent);
    }

    
    Revision::Ref CASRevisionStore::insertCAS(slice docID, generation cas,
                                              Revision::BodyParams body,
                                              Transaction &t)
    {
        CBFAssert(cas > 0);
        auto current = get(docID, KeyStore::kMetaOnly);
        if (!current || current->isFromCASServer()) {
            // Current version is from CAS server, or this doc doesn't exist yet:
            if (current && current->CAS() >= cas)
                return nullptr;

            // New rev is indeed newer, so save it as current:
            return writeCASRevision(current.get(), true, docID, cas, body, t);

        } else {
            // Current version is not from CAS server, so this creates a conflict.
            // Find the latest saved CAS version to replace:
            auto latest = getLatestCASServerRevision(docID);
            if (latest) {
                generation latestCAS = latest->CAS();
                if (latestCAS >= cas)
                    return nullptr;

                // If latest has the same CAS as the current rev, that means it's the saved
                // common ancestor; don't delete it.
                if (latestCAS != current->version().CASBase())
                    t(_nonCurrentStore).del(latest->document());
            }

            return writeCASRevision(latest.get(), false, docID, cas, body, t);
        }
    }


    void CASRevisionStore::assignCAS(Revision& rev, generation cas, Transaction &t) {
        CBFAssert(rev.isCurrent());
        generation parentCAS = rev.version().CASBase();
        rev.assignCAS(cas);
        t(_store).write(rev.document());

        if (parentCAS > 0) {
            t(_nonCurrentStore).del(keyForNonCurrentRevision(rev.docID(),
                                                             version(parentCAS, kCASServerPeerID)));
        }
    }



#pragma mark - OVERRIDDEN HOOKS:


    // Writes a revision from the CAS server to the current or non-current store:
    Revision::Ref CASRevisionStore::writeCASRevision(const Revision *parent,
                                                     bool current,
                                                     slice docID, generation cas,
                                                     Revision::BodyParams body,
                                                     Transaction &t)
    {
        VersionVector vers;
        if (parent)
            vers = parent->version();
        vers.setCAS(cas);
        Revision::Ref newRev {new Revision(docID, vers, body, current) };
        KeyStore &store = current ? *_db : _nonCurrentStore;
        t(store).write(newRev->document());
        return newRev;
    }


    // If a revision from the CAS server is being replaced by a newer revision that isn't,
    // back it up to the nonCurrent store so we can merge with it if necessary.
    void CASRevisionStore::backupCASVersion(Revision &curRev,
                                            const Revision &incomingRev,
                                            Transaction &t)
    {
        if (curRev.isFromCASServer() && !incomingRev.isFromCASServer()) {
            readBody(curRev);
            curRev.setCurrent(false);             // append the revID to the key
            t(_nonCurrentStore).write(curRev.document());
        }
    }


    // Is the current CAS-server backup revision (if any) now obsolete?
    // This happens when a revision from the CAS server replaces one that isn't.
    bool CASRevisionStore::shouldDeleteCASBackup(const Revision &newRev, const Revision *current) {
        return current && newRev.isFromCASServer() && !current->isFromCASServer();
    }


    // Is `rev` a saved CAS-server backup of the current revision `child`?
    bool CASRevisionStore::shouldKeepAncestor(const Revision &rev, const Revision &child) {
        generation cas = rev.version().current().CAS();
        return cas > 0 && cas == child.CAS();
    }

    
}
