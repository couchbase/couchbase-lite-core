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
#include "fleece/function_ref.hh"
#include "fleece/Fleece.hh"
#include "fleece/Expert.hh" // for SharedKeys
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>


namespace litecore { namespace repl {
    class ReplicatedRev;
    class UseCollection;

    using AccessLockedDB = access_lock<Retained<C4Database>>;

    /** Thread-safe access to a C4Database. */
    class DBAccess : public AccessLockedDB, public Logging {
    public:
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;
        using Dict = fleece::Dict;

        DBAccess(C4Database* db, bool disableBlobSupport);
        ~DBAccess();

        static inline void AssertDBOpen(const Retained<C4Database>& db) {
            if(!db) {
                litecore::error::_throw(litecore::error::Domain::LiteCore, litecore::error::LiteCoreError::NotOpen);
            }
        }

        /** Shuts down the DBAccess and makes further use of it invalid.  Any attempt to use
            it after this point is considered undefined behavior. */
        void close();
        
        /** Check the C4Collection inside the lock and returns a holder object that hodes this->useLocked().*/
        UseCollection       useCollection(C4Collection*);
        const UseCollection useCollection(C4Collection*) const;

        /** Looks up the remote DB identifier of this replication. */
        C4RemoteID lookUpRemoteDBID(slice key);

        /** Returns the remote DB identifier of this replication, once it's been looked up. */
        C4RemoteID remoteDBID() const                   {return _remoteDBID;}

        bool usingVersionVectors() const                {return _usingVersionVectors;}

        string convertVersionToAbsolute(slice revID);

        // (The "use" method is inherited from access_lock)

        //////// DOCUMENTS:

        /** Gets a document by ID */
        Retained<C4Document> getDoc(C4Collection* NONNULL,
                                    slice docID,
                                    C4DocContentLevel content) const;

        /** Returns the remote ancestor revision ID of a document. */
        alloc_slice getDocRemoteAncestor(C4Document *doc NONNULL);
        
        /** Updates the remote ancestor revision ID of a document, to an existing revision. */
        void setDocRemoteAncestor(C4Collection* NONNULL, slice docID, slice revID);
        
        /** Returns the document enumerator for all unresolved docs present in the DB */
        unique_ptr<C4DocEnumerator> unresolvedDocsEnumerator(C4Collection* NONNULL, bool orderByID);

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
        fleece::Doc applyDelta(C4Collection* NONNULL,
                               slice docID,
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
        std::atomic_flag _closed = ATOMIC_FLAG_INIT;
    };

    class UseCollection {
        DBAccess& _dbAccess;
        decltype(_dbAccess.useLocked()) _access;
        C4Collection* _collection;
    public:
        UseCollection(DBAccess& db_, C4Collection* collection)
        :_dbAccess(db_)
        ,_access(_dbAccess.useLocked())
        ,_collection(collection)
        {
            Assert(_access.get() == _collection->getDatabase());
        }
        C4Collection*       operator->()       { return _collection; }
        const C4Collection* operator->() const { return _collection; }
    };
} }
