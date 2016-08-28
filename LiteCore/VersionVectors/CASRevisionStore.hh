//
//  CASRevisionStore.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/11/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once
#include "RevisionStore.hh"

namespace litecore {


    /** RevisionStore that also supports revisions coming from a server that only tags documents
        with an integer clock (a "CAS" value) and doesn't support conflicts or merging. */
    class CASRevisionStore : public RevisionStore {
    public:

        explicit CASRevisionStore(DataFile *db);

        /** Returns the latest known revision from the CAS server. */
        Revision::Ref getLatestCASServerRevision(slice docID, generation &outCAS);

        /** Returns the base revision from the CAS server, the one the current rev is based on. */
        Revision::Ref getBaseCASServerRevision(slice docID, generation &outCAS);

        /** Inserts a new revision from the CAS server. */
        Revision::Ref insertFromServer(slice docID,
                                       generation cas,
                                       Revision::BodyParams,
                                       Transaction&);

        /** Assigns a revision a new CAS value after it's pushed to the CAS server.
            Also deletes the saved base & latest server revisions, if any. */
        void savedToCASServer(slice docID, slice revID, generation cas, Transaction &t);

        virtual Revision::Ref resolveConflict(std::vector<Revision*> conflicting,
                                              Revision::BodyParams body,
                                              Transaction &t) override;
        virtual void purge(slice docID, Transaction &t) override;

#if !DEBUG
    private:
#endif
        struct ServerState {
            struct Item {
                alloc_slice revID;
                generation CAS {0};

                Item() { }
                Item(slice r, generation g) :revID(r), CAS(g) { }
            };
            Item base;      // Common ancestor of local & server
            Item latest;    // Latest rev read from server (same as base except in conflicts)
        };
        ServerState getServerState(slice docID);
        void setServerState(slice docID, const ServerState&, Transaction &t);

    protected:
        virtual void willReplaceCurrentRevision(Revision &curRev, const Revision &incomingRev,
                                                Transaction &t) override;
        virtual bool shouldKeepAncestor(const Revision &rev) override;

    private:
        Revision::Ref writeCASRevision(const Revision *parent,
                                       bool current,
                                       slice docID,
                                       Revision::BodyParams body,
                                       Transaction &t);

        KeyStore &  _casStore;  // The KeyStore holding CAS metadata
    };

}
