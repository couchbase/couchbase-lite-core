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
#include "Fleece.hh"

namespace cbforest {


    CASRevisionStore::CASRevisionStore(Database *db)
    :RevisionStore(db),
     _casStore(db->getKeyStore("CAS"))
    { }



    CASRevisionStore::ServerState CASRevisionStore::getServerState(slice docID) {
        ServerState state;
        Document d = _casStore.get(docID);
        if (d.body().buf) {
            fleece::Array::iterator arr(fleece::Value::fromTrustedData(d.body())->asArray());
            if (arr.count() >= 2) {
                state.base.revID = arr[0]->asString();
                state.base.CAS   = arr[1]->asUnsigned();
            }
            if (arr.count() >= 4) {
                state.latest.revID = arr[2]->asString();
                state.latest.CAS   = arr[3]->asUnsigned();
            } else {
                state.latest = state.base;
            }
        }
        return state;
    }

    void CASRevisionStore::setServerState(slice docID,
                                          const CASRevisionStore::ServerState &state,
                                          Transaction &t)
    {
        fleece::Encoder enc;
        enc.beginArray();
        enc << state.base.revID << state.base.CAS;
        if (state.latest.revID.buf && state.latest.revID != state.base.revID)
            enc << state.latest.revID << state.latest.CAS;
        enc.endArray();
        t(_casStore).set(docID, slice::null, enc.extractOutput());
    }


    Revision::Ref CASRevisionStore::getLatestCASServerRevision(slice docID,
                                                               generation &outCAS) {
        auto serverState = getServerState(docID);
        if (!serverState.latest.revID.buf)
            return nullptr;
        outCAS = serverState.latest.CAS;
        return get(docID, serverState.latest.revID);
    }


    Revision::Ref CASRevisionStore::getBaseCASServerRevision(slice docID,
                                                             generation &outCAS) {
        auto serverState = getServerState(docID);
        if (!serverState.base.revID.buf)
            return nullptr;
        outCAS = serverState.base.CAS;
        return get(docID, serverState.base.revID);
    }

    
    Revision::Ref CASRevisionStore::insertFromServer(slice docID, generation cas,
                                                     Revision::BodyParams body,
                                                     Transaction &t)
    {
        CBFAssert(cas > 0);
        auto state = getServerState(docID);
        if (cas <= state.latest.CAS)
            return nullptr;

        Revision::Ref current;
        if (state.latest.CAS > 0)
            current = get(docID, KeyStore::kMetaOnly);

        Revision::Ref newRev;
        if (!current || current->revID() == state.latest.revID) {
            // Current version is from CAS server, or this doc doesn't exist yet,
            // so save this new revision as current:
            newRev = writeCASRevision(current.get(), true, docID, body, t);

        } else {
            // Current version is not from CAS server, so this creates a conflict.
            // Delete the latest saved server revision (or keep it as the base):
            Revision::Ref parent;
            if (state.latest.revID.buf) {
                parent = getNonCurrent(docID, state.latest.revID, KeyStore::kMetaOnly);
                if (state.latest.revID != state.base.revID)
                    deleteNonCurrent(docID, state.latest.revID, t);
            }
            // Create the new revision as a child of the latest:
            newRev = writeCASRevision(parent.get(), false, docID, body, t);
        }

        state.latest.revID = newRev->revID();
        state.latest.CAS = cas;
        if (!state.base.revID.buf)
            state.base = state.latest;
        setServerState(docID, state, t);
        return newRev;
    }


    void CASRevisionStore::assignCAS(slice docID, slice revID, generation cas, Transaction &t) {
        auto state = getServerState(docID);

        // Delete the saved base & latest server revisions:
        if (state.latest.revID.buf) {
            deleteNonCurrent(docID, state.latest.revID, t);
            if (state.base.revID != state.latest.revID)
                deleteNonCurrent(docID, state.base.revID, t);
        }
        // Update the saved state to point to the new revision as both base & latest:
        state.latest.revID = revID;
        state.latest.CAS = cas;
        state.base = state.latest;
        setServerState(docID, state, t);
    }


    // Writes a revision from the CAS server to the current or non-current store:
    Revision::Ref CASRevisionStore::writeCASRevision(const Revision *parent,
                                                     bool current,
                                                     slice docID,
                                                     Revision::BodyParams body,
                                                     Transaction &t)
    {
        VersionVector vers;
        if (parent)
            vers = parent->version();
        vers.incrementGen(kCASServerPeerID);
        Revision::Ref newRev { new Revision(docID, vers, body, current) };
        KeyStore &store = current ? *_db : _nonCurrentStore;
        t(store).write(newRev->document());
        return newRev;
    }


#pragma mark - OVERRIDDEN HOOKS:


    Revision::Ref CASRevisionStore::resolveConflict(std::vector<Revision*> conflicting,
                                                    Revision::BodyParams body,
                                                    Transaction &t)
    {
        slice docID = conflicting[0]->docID();
        auto state = getServerState(docID);

        // Don't delete the latest server rev after resolving the conflict:
        auto result = RevisionStore::resolveConflict(conflicting, state.latest.revID, body, t);

        if (state.base.revID != state.latest.revID) {
            // Update state to reflect that base server rev was deleted:
            state.base = state.latest;
            setServerState(docID, state, t);
        }
        return result;
    }

    void CASRevisionStore::willReplaceCurrentRevision(Revision &curRev,
                                            const Revision &incomingRev,
                                            Transaction &t)
    {
        // If a current revision from the CAS server is being replaced by a newer revision that
        // isn't, back it up to the nonCurrent store.
        if (incomingRev.version().current().author != kCASServerPeerID) {
            auto state = getServerState(curRev.docID());
            if (state.latest.revID == curRev.revID()) {
                readBody(curRev);
                curRev.setCurrent(false);             // append the revID to the key
                t(_nonCurrentStore).write(curRev.document());
            }
        }
    }


    // Is `rev` a saved CAS-server backup of the current revision `child`?
    bool CASRevisionStore::shouldKeepAncestor(const Revision &rev, const Revision &child) {
        auto state = getServerState(rev.docID());
        return rev.revID() == state.latest.revID || rev.revID() == state.base.revID;
    }

}
