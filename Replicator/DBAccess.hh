//
//  DBAccess.hh
//
//  Copyright (c) 2019 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
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

        DBAccess* newConnection(C4Error *outError);

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


        template <class LAMBDA>
        void useForInsert(LAMBDA callback) {
            insertionDB().use(callback);
        }

        template <class RESULT, class LAMBDA>
        RESULT useForInsert(LAMBDA callback) {
            return insertionDB().use<RESULT>(callback);
        }

        /** Manages a transaction safely. The begin() method calls beginTransaction, then commit()
            or abort() end it. If the object exits scope when it's been begun but not yet
            ended, it aborts the transaction. */
        class Transaction {
        public:
            Transaction(DBAccess &dba)
            :_dba(dba)
            { }

            ~Transaction() {
                if (_active)
                    abort(nullptr);
            }

            bool begin(C4Error *error) {
                assert(!_active);
                if (!_dba.beginTransaction(error))
                    return false;
                _active = true;
                return true;
            }

            bool end(bool commit, C4Error *error) {
                assert(_active);
                _active = false;
                return _dba.endTransaction(commit, error);
            }

            bool commit(C4Error *error)     {return end(true, error);}
            bool abort(C4Error *error)      {return end(false, error);}

            bool active() const             {return _active;}

        private:
            DBAccess &_dba;
            bool _active {false};
        };


    protected:
        virtual std::string loggingClassName() const override;
    private:
        friend class Transaction;
        
        void markRevsSyncedLater();
        fleece::SharedKeys tempSharedKeys();
        bool updateTempSharedKeys();
        bool beginTransaction(C4Error*);
        bool endTransaction(bool commit, C4Error*);
        access_lock<C4Database*>& insertionDB();

        C4BlobStore* const _blobStore;                      // Database's BlobStore
        fleece::SharedKeys _tempSharedKeys;                 // Keys used in tempEncodeJSON()
        std::mutex _tempSharedKeysMutex;                    // Mutex for replacing _tempSharedKeys
        unsigned _tempSharedKeysInitialCount {0};           // Count when copied from db's keys
        C4RemoteID _remoteDBID {0};                         // ID # of remote DB in revision store
        bool const _disableBlobSupport;                     // Does replicator support blobs?
        actor::Batcher<ReplicatedRev> _revsToMarkSynced;    // Pending revs to be marked as synced
        actor::Timer _timer;                                // Implements Batcher delay
        bool _inTransaction {false};
        std::unique_ptr<access_lock<C4Database*>> _insertionDB;
    };

} }
