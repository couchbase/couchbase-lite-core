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
        Revision::Ref cur;
        auto e = enumerateRevisions(docID, kCASServerPeerID);
        while (e.next()) {
            Revision rev(e.moveDoc());
            if (!cur || rev.version().CAS() > cur->version().CAS())
                cur.reset(new Revision(std::move(rev)));
        }
        return cur;
        //OPT: Wouldn't have to compare revs if the gen numbers were ordered in the keys.
    }


    versionOrder CASRevisionStore::insertCAS(slice docID, generation cas,
                                   Revision::BodyParams body,
                                   Transaction &t)
    {
        CBFAssert(cas > 0);
        auto current = get(docID, KeyStore::kMetaOnly);
        generation currentCAS = current ? current->version().CAS() : 0;
        if (!current || current->version().isFromCASServer()) {
            // Current version is from CAS server, or this doc doesn't exist yet:
            versionOrder o = version::compareGen(cas, currentCAS);
            if (o != kNewer)
                return o;

            // New rev is indeed newer, so save it as current:
            writeCASRevision(current.get(), true, docID, cas, body, t);
            return kNewer;

        } else {
            // Current version is not from CAS server, so this creates a conflict.
            // Find the latest saved CAS version to replace:
            auto latest = getLatestCASServerRevision(docID);
            if (latest) {
                generation latestCAS = latest->version().CAS();
                versionOrder o = version::compareGen(cas, latestCAS);
                if (o != kNewer)
                    return o;

                // If latest has the same CAS as the current rev, that means it's the saved
                // common ancestor; don't delete it.
                if (latestCAS != currentCAS)
                    t(_nonCurrentStore).del(latest->document());
            }

            writeCASRevision(latest.get(), false, docID, cas, body, t);
            return kNewer;
        }
    }


#pragma mark - OVERRIDDEN HOOKS:


    // Writes a revision from the CAS server to the current or non-current store:
    void CASRevisionStore::writeCASRevision(const Revision *parent,
                                            bool current,
                                            slice docID, generation cas,
                                            Revision::BodyParams body,
                                            Transaction &t)
    {
        VersionVector vers;
        if (parent)
            vers = parent->version();
        vers.setCAS(cas);
        Revision newRev(docID, vers, body, current);
        KeyStore &store = current ? *_db : _nonCurrentStore;
        t(store).write(newRev.document());
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
        return cas > 0 && cas == child.version().CAS();
    }

    
}
