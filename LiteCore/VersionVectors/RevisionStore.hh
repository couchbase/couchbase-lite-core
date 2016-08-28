//
//  RevisionStore.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/8/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once
#include "DataFile.hh"
#include "Revision.hh"
#include "VersionVector.hh"

class RevisionStoreTest;

namespace litecore {

    class Revision;


    /** Manages storage of version-vectored document revisions (Revision objects) in a DataFile. */
    class RevisionStore {
    public:

        RevisionStore(DataFile *db, peerID myPeerID);
        virtual ~RevisionStore() { }

        const alloc_slice& myPeerID() const                       {return _myPeerID;}

        //////// GETTING REVISIONS:

        /** Returns the current revision of a document, or null. */
        Revision::Ref get(slice docID,
                      ContentOptions = kDefaultContent) const;

        /** Returns a specific revision of a document, or null.
            (If revID is a null slice, returns the current revision.) */
        Revision::Ref get(slice docID, slice revID,
                      ContentOptions = kDefaultContent) const;

        /** Loads the body of a Revision (if it was originally loaded as meta-only) */
        void readBody(Revision&);

        /** How does this revision compare to what's in the database?
            @return  kNewer if it should be added, kSame if it's present, kOlder if it's obsolete. */
        versionOrder checkRevision(slice docID, slice revID);

        /** Returns all the non-current (conflicting or ancestor) revisions of the given doc. */
        std::vector<std::shared_ptr<Revision> > allOtherRevisions(slice docID);

        //////// ADDING REVISIONS:

        /** Creates a new revision.
            @param docID  The document ID
            @param parentVersion  The version vector of the revision being modified, empty if new
            @param body  The body and related flags
            @param t  Transaction to write the revision to
            @return  New Revision, or null if there's a conflict */
        Revision::Ref create(slice docID,
                             const VersionVector &parentVersion,
                             Revision::BodyParams body,
                             Transaction &t);

        /** Inserts a revision, probably from a peer. */
        versionOrder insert(Revision&, Transaction&);

        /** Creates a new revision that resolves a conflict.
            @param conflicting  The two or more conflicting revisions
            @param body  The body of the merged revision
            @param t  A Transaction to use to write the revision
            @return  The saved merged revision. */
        virtual Revision::Ref resolveConflict(std::vector<Revision*> conflicting,
                                              Revision::BodyParams body,
                                              Transaction &t);

        /** Remove a document entirely from storage. */
        virtual void purge(slice docID, Transaction &t);

    protected:
        //////// DOCUMENT KEYS:

        /** The document key to use for a non-current Revision. */
        static alloc_slice keyForNonCurrentRevision(slice docID, class Version vers);

        /** The start of the key range for non-current Revisions with the given docID
            (and author, if non-null.) */
        static alloc_slice startKeyFor(slice docID, peerID author =slice::null);
        
        /** The non-inclusive end of the key range for non-current Revisions with the given docID
            (and author, if non-null.) */
        static alloc_slice endKeyFor(slice docID, peerID author =slice::null);

        /** Given a key in the nonCurrentStore, returns the ID of the document. */
        static slice docIDFromKey(slice key);

        Revision::Ref resolveConflict(std::vector<Revision*> conflicting,
                                      slice keepingRevID,
                                      Revision::BodyParams body,
                                      Transaction &t);
        void markConflicted(Revision &current, bool conflicted, Transaction &t);
        bool hasConflictingRevisions(slice docID);
        void replaceCurrent(Revision &newRev, Revision *current, Transaction &t);
        bool deleteNonCurrent(slice docID, slice revID, Transaction &t);
        Revision::Ref getNonCurrent(slice docID, slice revID, ContentOptions) const;
        void deleteAncestors(Revision&, Transaction&);
        DocEnumerator enumerateRevisions(slice docID, slice author = slice::null);

        //////// SUBCLASS HOOKS:

        virtual void willReplaceCurrentRevision(Revision &curRev, const Revision &incomingRev,
                                                Transaction &t);
        virtual bool shouldKeepAncestor(const Revision &rev);

    protected:
        KeyStore &  _currentStore;      // The KeyStore where current revisons are stored
        KeyStore &  _nonCurrentStore;   // The KeyStore for non-current revisions
        alloc_slice _myPeerID;          // The local peerID, represented by "*" in Revisions.

        friend class Revision;
        friend class ::RevisionStoreTest;
    };

}
