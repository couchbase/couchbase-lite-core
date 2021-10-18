//
//  DBAccess.hh
//
//  Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Collection.hh"
#include "c4Database.hh"
#include "c4Document.hh"
#include "Batcher.hh"
#include "Logging.hh"
#include "Timer.hh"
#include "access_lock.hh"
#include "function_ref.hh"
#include "fleece/Fleece.hh"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>


namespace litecore { namespace repl {
    class ReplicatedRev;

    using AccessLockedDB = access_lock<Retained<C4Database>>;

    /** Thread-safe access to a C4Database. */
    class DBAccess : public AccessLockedDB, public Logging {
    public:
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;
        using Dict = fleece::Dict;

        DBAccess(C4Database* db, bool disableBlobSupport);
        ~DBAccess();

        /** Looks up the remote DB identifier of this replication. */
        C4RemoteID lookUpRemoteDBID(slice key);

        /** Returns the remote DB identifier of this replication, once it's been looked up. */
        C4RemoteID remoteDBID() const                   {return _remoteDBID;}

        bool usingVersionVectors() const                {return _usingVersionVectors;}

        string convertVersionToAbsolute(slice revID);

        // (The "use" method is inherited from access_lock)

        //////// DOCUMENTS:

        /** Gets a document by ID */
        Retained<C4Document> getDoc(slice docID, C4DocContentLevel content) const {
            return useLocked()->getDocument(docID, true, content);
        }

        /** Returns the remote ancestor revision ID of a document. */
        alloc_slice getDocRemoteAncestor(C4Document *doc NONNULL);
        
        /** Updates the remote ancestor revision ID of a document, to an existing revision. */
        void setDocRemoteAncestor(slice docID, slice revID);
        
        /** Returns the document enumerator for all unresolved docs present in the DB */
        unique_ptr<C4DocEnumerator> unresolvedDocsEnumerator(bool orderByID);

         /** Mark this revision as synced (i.e. the server's current revision) soon.
             NOTE: While this is queued, calls to C4Document::getRemoteAncestor() for this doc won't
             return the correct answer, because the change hasn't been made in the database yet.
             For that reason, you must ensure that markRevsSyncedNow() is called before any call
             to C4Document::getRemoteAncestor(). */
        void markRevSynced(ReplicatedRev *rev NONNULL);

        /** Synchronously fulfills all markRevSynced requests. */
        void markRevsSyncedNow();

        //////// DELTAS:

        /** Applies a delta to an existing revision. Never returns NULL;
            errors decoding or applying the delta are thrown as Fleece exceptions. */
        fleece::Doc applyDelta(C4Document *doc NONNULL,
                               slice deltaJSON,
                               bool useDBSharedKeys);

        /** Reads a document revision and applies a delta to it.
            Returns NULL if the baseRevID no longer exists or its body is not known.
            Other errors (including doc-not-found) are thrown as exceptions. */
        fleece::Doc applyDelta(slice docID,
                               slice baseRevID,
                               slice deltaJSON);

        //////// BLOBS / ATTACHMENTS:

        /** The blob store is thread-safe so it can be accessed directly. */
        C4BlobStore* blobStore() const                  {return _blobStore;}

        /** True if the DB should store `_attachments` properties */
        bool disableBlobSupport() const                 {return _disableBlobSupport;}

        using FindBlobCallback = fleece::function_ref<void(FLDeepIterator,
                                                           Dict blob,
                                                           const C4BlobKey &key)>;
        /** Finds all blob references in the dict, at any depth. */
        void findBlobReferences(Dict root,
                                bool unique,
                                const FindBlobCallback &callback);

        /** Writes `root` to the encoder, transforming blobs into old-school `_attachments` dict */
        void encodeRevWithLegacyAttachments(fleece::Encoder& enc,
                                           Dict root,
                                           unsigned revpos);

        //////// INSERTION:

        /** Encodes JSON to Fleece. Uses a temporary SharedKeys, because the database's
            SharedKeys can only be encoded with during a transaction, and the caller (IncomingRev)
            isn't in a transaction. */
        fleece::Doc tempEncodeJSON(slice jsonBody, FLError *err);

        /** Takes a document produced by tempEncodeJSON and re-encodes it if necessary with the
            database's real SharedKeys, so it's suitable for saving. This can only be called
            inside a transaction. */
        alloc_slice reEncodeForDatabase(fleece::Doc);

        /** A separate C4Database instance used for insertions, to avoid blocking the main
            C4Database. */
        AccessLockedDB& insertionDB();


        /** Manages a transaction safely. The begin() method calls beginTransaction, then commit()
             or abort() end it. If the object exits scope when it's been begun but not yet
             ended, it aborts the transaction. */
        class Transaction {
        public:
            Transaction(AccessLockedDB &dba)
            :_dba(dba.useLocked())
            ,_t(_dba)
            { }

            void commit()     {_t.commit();}
            void abort()      {_t.abort();}

        private:
            AccessLockedDB::access<Retained<C4Database>&> _dba;
            C4Database::Transaction _t;
            bool _active {false};
        };



        static std::atomic<unsigned> gNumDeltasApplied;  // For unit tests only

    private:
        void markRevsSyncedLater();
        fleece::SharedKeys tempSharedKeys();
        fleece::SharedKeys updateTempSharedKeys();

        C4BlobStore* const _blobStore;                      // Database's BlobStore
        fleece::SharedKeys _tempSharedKeys;                 // Keys used in tempEncodeJSON()
        std::mutex _tempSharedKeysMutex;                    // Mutex for replacing _tempSharedKeys
        unsigned _tempSharedKeysInitialCount {0};           // Count when copied from db's keys
        C4RemoteID _remoteDBID {0};                         // ID # of remote DB in revision store
        alloc_slice _remotePeerID;                          // peerID of remote peer
        bool const _disableBlobSupport;                     // Does replicator support blobs?
        actor::Batcher<ReplicatedRev> _revsToMarkSynced;    // Pending revs to be marked as synced
        actor::Timer _timer;                                // Implements Batcher delay
        std::optional<AccessLockedDB> _insertionDB;         // DB handle to use for insertions
        std::string _myPeerID;
        const bool _usingVersionVectors;                    // True if DB uses version vectors
    };

} }
