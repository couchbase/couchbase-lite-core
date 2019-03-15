//
//  DBAccess.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/20/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "c4.hh"
#include "Batcher.hh"
#include "Logging.hh"
#include "Timer.hh"
#include "access_lock.hh"
#include "function_ref.hh"
#include "fleece/Fleece.hh"


namespace litecore { namespace repl {
    class ReplicatedRev;

    /** Thread-safe access to a C4Database */
    class DBAccess : public access_lock<C4Database*>, public Logging {
    public:
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;
        using Dict = fleece::Dict;

        DBAccess(C4Database* db, bool disableBlobSupport);
        ~DBAccess();

        C4Document* getDoc(slice docID, C4Error *outError) const {
            return use<C4Document*>([&](C4Database *db) {
                return c4doc_get(db, docID, true, outError);
            });
        }

        C4RawDocument* getRawDoc(slice storeID, slice docID, C4Error *outError) const {
            return use<C4RawDocument*>([&](C4Database *db) {
                return c4raw_get(db, storeID, docID, outError);
            });
        }

        static Dict getDocRoot(C4Document *doc,
                               C4RevisionFlags *outFlags =nullptr);

        static Dict getDocRoot(C4Document *doc, slice revID,
                               C4RevisionFlags *outFlags =nullptr);

        C4RemoteID lookUpRemoteDBID(slice key, C4Error *outError);
        C4RemoteID remoteDBID() const                   {return _remoteDBID;}

        alloc_slice getDocRemoteAncestor(C4Document *doc);

         /** Mark this revision as synced (i.e. the server's current revision) soon.
             NOTE: While this is queued, calls to c4doc_getRemoteAncestor() for this document won't
             return the correct answer, because the change hasn't been made in the database yet.
             For that reason, you must ensure that markRevsSyncedNow() is called before any call
             to c4doc_getRemoteAncestor(). */
        void markRevSynced(ReplicatedRev *rev)          {_revsToMarkSynced.push(rev);}

        void markRevsSyncedNow();

        /** The blob store is thread-safe so it can be accessed directly. */
        C4BlobStore* blobStore() const                  {return _blobStore;}
        bool disableBlobSupport() const                 {return _disableBlobSupport;}

        using FindBlobCallback = fleece::function_ref<void(FLDeepIterator,
                                                           Dict blob,
                                                           const C4BlobKey &key)>;
        /** Encodes JSON to Fleece. Uses a temporary SharedKeys, because the database's
            SharedKeys can only be encoded with during a transaction, and the caller (IncomingRev)
            isn't in a transaction. */
        fleece::Doc tempEncodeJSON(slice jsonBody, FLError *err);

        /** Takes a document produced by tempEncodeJSON and re-encodes it if necessary with the
            database's real SharedKeys, so it's suitable for saving. This can only be called
            inside a transaction. */
        alloc_slice reEncodeForDatabase(fleece::Doc);

        /** Calls the callback inside a database transaction.
            Callback should look like: (C4Database*,C4Error*)->bool */
        template <class LAMBDA>
        C4Error inTransaction(LAMBDA callback) {
            C4Error err;
            use([&](C4Database *db) {
                c4::Transaction transaction(db);
                if (transaction.begin(&err) && callback(db, &err) && transaction.commit(&err)) {
                    updateTempSharedKeys();
                    err = {};
                }
            });
            return err;
        }

        /** Applies a delta to an existing revision. */
        fleece::Doc applyDelta(const C4Revision *baseRevision,
                               slice deltaJSON,
                               bool useDBSharedKeys,
                               C4Error *outError);

        fleece::Doc applyDelta(slice docID,
                               slice baseRevID,
                               slice deltaJSON,
                               C4Error *outError);

        void findBlobReferences(Dict root,
                                bool unique,
                                const FindBlobCallback &callback);

        /** Writes `root` to the encoder, transforming blobs into old-school `_attachments` dict */
        void encodeRevWithLegacyAttachments(fleece::Encoder& enc,
                                           Dict root,
                                           unsigned revpos);

        static std::atomic<unsigned> gNumDeltasApplied;  // For unit tests only

    protected:
        virtual std::string loggingClassName() const override;
    private:
        void markRevsSyncedLater();
        fleece::SharedKeys tempSharedKeys();
        bool updateTempSharedKeys();

        C4BlobStore* const _blobStore;                      // Database's BlobStore
        fleece::SharedKeys _tempSharedKeys;                 // Keys used in tempEncodeJSON()
        std::mutex _tempSharedKeysMutex;                    // Mutex for replacing _tempSharedKeys
        unsigned _tempSharedKeysInitialCount {0};           // Count when copied from db's keys
        C4RemoteID _remoteDBID {0};                         // ID # of remote DB in revision store
        bool const _disableBlobSupport;                     // Does replicator support blobs?
        actor::Batcher<ReplicatedRev> _revsToMarkSynced;    // Pending revs to be marked as synced
        actor::Timer _timer;                                // Implements Batcher delay
    };

} }
