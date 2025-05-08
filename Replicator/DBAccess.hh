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
#include "DatabasePool.hh"
#include "EchoCanceler.hh"
#include "Error.hh"
#include "Logging.hh"
#include "Timer.hh"
#include "access_lock.hh"
#include "fleece/function_ref.hh"
#include "fleece/Fleece.hh"
#include "fleece/Expert.hh"  // for SharedKeys
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace litecore::repl {
    using fleece::Retained;
    class ReplicatedRev;
    class UseCollection;

    /** Thread-safe access to a C4Database. */
    class DBAccess final : public Logging {
      public:
        using slice       = fleece::slice;
        using alloc_slice = fleece::alloc_slice;
        using Dict        = fleece::Dict;

        DBAccess(DatabasePool*, bool disableBlobSupport);
        DBAccess(C4Database*, bool disableBlobSupport);
        ~DBAccess() override;

        /// Returns a temporary object convertible to C4Database*. Use it only briefly.
        BorrowedDatabase useLocked() const { return _pool->borrow(); }

        /// Returns a temporary object convertible to C4Collection*. Use it only briefly.
        BorrowedCollection useCollection(C4CollectionSpec const& spec) const {
            return BorrowedCollection(_pool->borrow(), spec);
        }

        auto useLocked(auto callback) const {
            BorrowedDatabase db = _pool->borrow();
            return callback(db.get());
        }

        /// Returns a writeable database. Use only when you need to write.
        BorrowedDatabase useWriteable() { return _pool->borrowWriteable(); }

        /** Shuts down the DBAccess and makes further use of it invalid.  Any attempt to use
            it after this point is considered undefined behavior. */
        void close();

        /** Looks up the remote DB identifier of this replication. */
        C4RemoteID lookUpRemoteDBID(slice key);

        /** Returns the remote DB identifier of this replication, once it's been looked up. */
        C4RemoteID remoteDBID() const { return _remoteDBID; }

        bool usingVersionVectors() const { return _usingVersionVectors; }

        std::string convertVersionToAbsolute(slice revID);

        //////// DOCUMENTS:

        /** Gets a document by ID */
        Retained<C4Document> getDoc(C4CollectionSpec const&, slice docID, C4DocContentLevel content) const;

        /** Returns the remote ancestor revision ID of a document. */
        alloc_slice getDocRemoteAncestor(C4Document* doc NONNULL) const;

        /** Updates the remote ancestor revision ID of a document, to an existing revision. */
        void setDocRemoteAncestor(C4CollectionSpec const&, slice docID, slice revID);

        /** Returns the document enumerator for all unresolved docs present in the DB */
        static std::unique_ptr<C4DocEnumerator> unresolvedDocsEnumerator(C4Collection*, bool orderByID);

        /** Mark this revision as synced (i.e. the server's current revision) soon.
             NOTE: While this is queued, calls to C4Document::getRemoteAncestor() for this doc won't
             return the correct answer, because the change hasn't been made in the database yet.
             For that reason, you must ensure that markRevsSyncedNow() is called before any call
             to C4Document::getRemoteAncestor(). */
        void markRevSynced(ReplicatedRev* rev NONNULL);

        /** Synchronously fulfills all markRevSynced requests. */
        void markRevsSyncedNow();
        void markRevsSyncedNow(C4Database* db);

        /** Prevents the ChangesFeed from "echoing" revisions just added by the Inserter. */
        EchoCanceler echoCanceler;

        //////// DELTAS:

        /** Applies a delta to an existing revision. Never returns NULL;
            errors decoding or applying the delta are thrown as Fleece exceptions.
            If `db` is non-null, the doc will be re-encoded with its SharedKeys. */
        fleece::Doc applyDelta(C4Document* doc NONNULL, slice deltaJSON, C4Database* db);

        /** Reads a document revision and applies a delta to it.
            Returns NULL if the baseRevID no longer exists or its body is not known.
            Other errors (including doc-not-found) are thrown as exceptions. */
        fleece::Doc applyDelta(C4CollectionSpec const&, slice docID, slice baseRevID, slice deltaJSON);

        //////// BLOBS / ATTACHMENTS:

        /** The blob store is thread-safe so it can be accessed directly. */
        C4BlobStore* blobStore() const { return _blobStore; }

        /** True if the DB should store `_attachments` properties */
        bool disableBlobSupport() const { return _disableBlobSupport; }

        using FindBlobCallback = fleece::function_ref<void(FLDeepIterator, Dict blob, const C4BlobKey& key)>;
        /** Finds all blob references in the dict, at any depth. */
        void findBlobReferences(Dict root, bool unique, const FindBlobCallback& callback) const;
        bool hasBlobReferences(Dict root) const;

        /** Writes `root` to the encoder, transforming blobs into old-school `_attachments` dict */
        void encodeRevWithLegacyAttachments(fleece::Encoder& enc, Dict root, unsigned revpos) const;

        //////// INSERTION:

        /** Encodes JSON to Fleece. Uses a temporary SharedKeys, because the database's
            SharedKeys can only be encoded with during a transaction, and the caller (IncomingRev)
            isn't in a transaction. */
        fleece::Doc tempEncodeJSON(slice jsonBody, FLError* err);

        /** Takes a document produced by tempEncodeJSON and re-encodes it if necessary with the
            database's real SharedKeys, so it's suitable for saving. This can only be called
            inside a transaction. */
        alloc_slice reEncodeForDatabase(fleece::Doc, C4Database*);

        /** Manages a transaction safely. Call commit() to commit, abort() to abort.
            If the object exits scope when it's been begun but not yet ended, it aborts the transaction. */
        class Transaction {
          public:
            explicit Transaction(DBAccess& dba) : _db(dba.useWriteable()), _t(_db) {}

            C4Database* db() { return _db.get(); }

            void commit() { _t.commit(); }

            void abort() { _t.abort(); }

          private:
            BorrowedDatabase        _db;
            C4Database::Transaction _t;
        };

        static std::atomic<unsigned> gNumDeltasApplied;  // For unit tests only

      protected:
        std::string loggingClassName() const override { return "DBAccess"; }

      private:
        void               markRevsSyncedLater();
        fleece::SharedKeys tempSharedKeys();
        fleece::SharedKeys updateTempSharedKeys();

        Retained<DatabasePool>        _pool;                           // Pool of C4Databases
        C4BlobStore*                  _blobStore{};                    // Database's BlobStore
        fleece::SharedKeys            _tempSharedKeys;                 // Keys used in tempEncodeJSON()
        std::mutex                    _tempSharedKeysMutex;            // Mutex for replacing _tempSharedKeys
        unsigned                      _tempSharedKeysInitialCount{0};  // Count when copied from db's keys
        C4RemoteID                    _remoteDBID{0};                  // ID # of remote DB in revision store
        alloc_slice                   _remoteSourceID;                 // SourceID of remote peer
        bool const                    _disableBlobSupport;             // Does replicator support blobs?
        actor::Batcher<ReplicatedRev> _revsToMarkSynced;               // Pending revs to be marked as synced
        actor::Timer                  _timer;                          // Implements Batcher delay
        std::string                   _mySourceID;                     // Version vector sourceID
        const bool                    _usingVersionVectors;            // True if DB uses version vectors
        bool                          _ownsPool = false;               // True if I created _pool
        std::atomic_flag              _closed   = ATOMIC_FLAG_INIT;    // True after closed
    };

}  // namespace litecore::repl
