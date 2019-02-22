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

        static fleece::Dict getDocRoot(C4Document *doc,
                                       C4RevisionFlags *outFlags =nullptr);

        static fleece::Dict getDocRoot(C4Document *doc, slice revID,
                                       C4RevisionFlags *outFlags =nullptr);

        C4RemoteID lookUpRemoteDBID(slice key, C4Error *outError);
        C4RemoteID remoteDBID() const                   {return _remoteDBID;}

        fleece::alloc_slice getDocRemoteAncestor(C4Document *doc);

        // Mark this revision as synced (i.e. the server's current revision) soon.
        // NOTE: While this is queued, calls to c4doc_getRemoteAncestor() for this document won't
        // return the correct answer, because the change hasn't been made in the database yet.
        // For that reason, you must ensure that markRevsSyncedNow() is called before any call
        // to c4doc_getRemoteAncestor().
        void markRevSynced(ReplicatedRev *rev)          {_revsToMarkSynced.push(rev);}

        void markRevsSyncedNow();

        /** The blob store is thread-safe so it can be accessed directly. */
        C4BlobStore* blobStore() const                  {return _blobStore;}
        bool disableBlobSupport() const                 {return _disableBlobSupport;}

        using FindBlobCallback = fleece::function_ref<void(FLDeepIterator,
                                                           fleece::Dict blob,
                                                           const C4BlobKey &key)>;
        void findBlobReferences(fleece::Dict root,
                                bool unique,
                                const FindBlobCallback &callback);

        void writeRevWithLegacyAttachments(fleece::Encoder& enc, fleece::Dict root, unsigned revpos);

    protected:
        virtual std::string loggingClassName() const override;
    private:
        void markRevsSyncedLater();

        C4BlobStore* const _blobStore;
        C4RemoteID _remoteDBID {0};                 // ID # of remote DB in revision store
        bool const _disableBlobSupport;
        actor::Batcher<ReplicatedRev> _revsToMarkSynced;     // Pending revs to be marked as synced
        actor::Timer _timer;
    };

} }
