//
// CollectionImpl.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Collection.hh"
#include "c4BlobStore.hh"
#include "c4Database.hh"
#include "c4Document.hh"
#include "c4ExceptionUtils.hh"
#include "c4Index.hh"
#include "c4Internal.hh"
#include "c4Observer.hh"
#include "DatabaseImpl.hh"
#include "TreeDocument.hh"
#include "VectorDocument.hh"
#include "SequenceTracker.hh"
#include "Housekeeper.hh"
#include "KeyStore.hh"
#include "SQLiteDataFile.hh"
#include "RevTree.hh"
#include "access_lock.hh"
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "betterassert.hh"

namespace litecore {

    class CollectionImpl final
        : public C4Collection
        , public Logging {
      public:
        CollectionImpl(C4Database* db, C4CollectionSpec spec, KeyStore& store)
            : C4Collection(db, spec), Logging(DBLog), _keyStore(&store) {
            auto flags = _database->getConfiguration().flags;
            if ( flags & kC4DB_VersionVectors ) _documentFactory = std::make_unique<VectorDocumentFactory>(this);
            else
                _documentFactory = std::make_unique<TreeDocumentFactory>(this);

            if ( !(_database->getConfiguration().flags & kC4DB_NonObservable) )
                _sequenceTracker = std::make_unique<access_lock<SequenceTracker>>(SequenceTracker(store.name()));

            DatabaseImpl* dbImpl = asInternal(db);
            logInfo("DB=%s Instantiated", dbImpl->dataFile()->loggingName().c_str());
        }

        ~CollectionImpl() override { destructExtraInfo(_extraInfo); }

        void close() {
            logInfo("Closing");
            stopHousekeeping();
            _sequenceTracker = nullptr;
            _documentFactory = nullptr;
            _keyStore        = nullptr;
            _database        = nullptr;
            logInfo("Closed");
        }

        std::string fullName() const {
            std::string name;
            auto        spec = getSpec();
            if ( spec.scope != kC4DefaultScopeID ) (name += std::string_view(slice(spec.scope))) += "/";
            name += std::string_view(slice(spec.name));
            return name;
        }

        std::string loggingClassName() const override { return "Collection"; }

        std::string loggingIdentifier() const override {  // Logging API
            if ( _usuallyFalse(!isValid()) ) { return format("Closed collection %.*s", SPLAT(_name)); }

            auto dbName = _database->getName();
            return format("%.*s/%.*s", SPLAT(dbName), SPLAT(_name));
        }

        KeyStore& keyStore() const {
            if ( _usuallyFalse(!isValid()) ) failClosed();
            return *_keyStore;
        }

        uint64_t getDocumentCount() const override { return keyStore().recordCount(); }

        C4SequenceNumber getLastSequence() const override { return keyStore().lastSequence(); }

        DatabaseImpl* dbImpl() { return asInternal(getDatabase()); }

        const DatabaseImpl* dbImpl() const { return asInternal(getDatabase()); }

        access_lock<SequenceTracker>& sequenceTracker() {
            if ( !_sequenceTracker ) error::_throw(error::UnsupportedOperation);
            return *_sequenceTracker;
        }

#pragma mark - TRANSACTIONS:

        void transactionBegan() {
            if ( _sequenceTracker ) _sequenceTracker->useLocked()->beginTransaction();
        }

        bool changedDuringTransaction() {
            return _sequenceTracker && _sequenceTracker->useLocked()->changedDuringTransaction();
        }

        void transactionEnding(ExclusiveTransaction* transaction, bool committing) {
            if ( _sequenceTracker ) {
                auto st = _sequenceTracker->useLocked();
                // Notify other Database instances on this file:
                if ( committing && st->changedDuringTransaction() ) transaction->notifyCommitted(st);
                st->endTransaction(committing);
            }
        }

        void externalTransactionCommitted(const SequenceTracker& sourceTracker) {
            if ( _sequenceTracker ) _sequenceTracker->useLocked()->addExternalTransaction(sourceTracker);
        }

#pragma mark - BLOBS:

        void findBlobReferences(const fleece::function_ref<bool(FLDict)>& blobCallback) override {
            uint64_t                  numRevisions = 0;
            RecordEnumerator::Options options;
            options.onlyBlobs  = true;
            options.sortOption = kUnsorted;
            RecordEnumerator e(keyStore(), options);
            while ( e.next() ) {
                Retained<C4Document> doc = _documentFactory->newDocumentInstance(e.record());
                doc->selectCurrentRevision();
                do {
                    if ( doc->loadRevisionBody() ) {
                        FLDict body = doc->getProperties();
                        C4Blob::findBlobReferences(body, blobCallback);
                        C4Blob::findAttachmentReferences(body, blobCallback);
                        ++numRevisions;
                    }
                } while ( doc->selectNextRevision() );
            }
        }

#pragma mark - DOCUMENTS:

        DocumentFactory* documentFactory() const {
            if ( _usuallyFalse(!isValid()) ) failClosed();
            return _documentFactory.get();
        }

        virtual Retained<C4Document> newDocumentInstance(const litecore::Record& record) {
            return documentFactory()->newDocumentInstance(record);
        }

        Retained<C4Document> getDocument(slice docID, bool mustExist, C4DocContentLevel content) const override {
            auto doc = documentFactory()->newDocumentInstance(docID, ContentOption(content));
            if ( mustExist && !doc->exists() ) doc = nullptr;
            return doc;
        }

        Retained<C4Document> getDocumentBySequence(C4SequenceNumber sequence) const override {
            if ( Record rec = keyStore().get(sequence, kEntireBody); rec.exists() )
                return documentFactory()->newDocumentInstance(rec);
            else
                return nullptr;
        }

        std::vector<alloc_slice> findDocAncestors(const std::vector<slice>& docIDs, const std::vector<slice>& revIDs,
                                                  unsigned maxAncestors, bool mustHaveBodies,
                                                  C4RemoteID remoteDBID) const override {
            return documentFactory()->findAncestors(docIDs, revIDs, maxAncestors, mustHaveBodies, remoteDBID);
        }

        // Errors other than NotFound, Conflict and delta failures
        // should be thrown as exceptions, in the C++ API.
        static void throwIfUnexpected(const C4Error& inError, C4Error* outError) {
            if ( outError ) *outError = inError;
            if ( inError.domain == LiteCoreDomain ) {
                switch ( inError.code ) {
                    case kC4ErrorNotFound:
                    case kC4ErrorConflict:
                    case kC4ErrorDeltaBaseUnknown:
                    case kC4ErrorCorruptDelta:
                        return;  // don't throw these errors
                }
            }
            C4Error::raise(inError);
        }

        bool markDocumentSynced(slice docID, slice revID, C4SequenceNumber sequence, C4RemoteID remoteID) override {
            if ( remoteID == RevTree::kDefaultRemoteID ) {
                // Shortcut: can set kSynced flag on the record to mark that the current revision is
                // synced to remote #1. But the call will return false if the sequence no longer
                // matches, i.e this revision is no longer current. Then have to take the slow approach.
                if ( keyStore().setDocumentFlag(docID, sequence, DocumentFlags::kSynced, dbImpl()->transaction()) ) {
                    return true;
                }
            }

            // Slow path: Load the doc and update the remote-ancestor info in the rev tree:
            Retained<C4Document> doc = getDocument(docID, true, kDocGetAll);
            if ( !doc ) return false;
            if ( !revID ) {
                // Look up revID by sequence, if it wasn't given:
                Assert(sequence != 0_seq);
                do {
                    if ( doc->selectedRev().sequence == sequence ) {
                        revID = doc->selectedRev().revID;
                        break;
                    }
                } while ( doc->selectNextRevision() );
                if ( !revID ) return false;
            }
            if ( remoteID == RevTree::kNoRemoteID ) {
                doc->revIsRejected(revID);
            } else {
                doc->setRemoteAncestorRevID(remoteID, revID);
            }
            doc->save();
            return true;
        }

        Retained<C4Document> createDocument(slice docID, slice revBody, C4RevisionFlags revFlags,
                                            C4Error* outError) override {
            C4DocPutRequest rq = {};
            rq.docID           = docID;
            rq.body            = revBody;
            rq.revFlags        = revFlags;
            rq.save            = true;
            return putDocument(rq, nullptr, outError);
        }

        Retained<C4Document> putDocument(const C4DocPutRequest& rq, size_t* outCommonAncestorIndex,
                                         C4Error* outError) override {
            dbImpl()->mustBeInTransaction();
            if ( rq.docID.buf && !C4Document::isValidDocID(rq.docID) ) error::_throw(error::BadDocID);
            if ( rq.existingRevision || rq.historyCount > 0 ) AssertParam(rq.docID.buf, "Missing docID");
            if ( rq.existingRevision ) {
                AssertParam(rq.historyCount > 0, "No history");
            } else {
                AssertParam(rq.historyCount <= 1, "Too much history");
                AssertParam(rq.historyCount > 0 || !(rq.revFlags & kRevDeleted),
                            "Can't create a new already-deleted document");
                AssertParam(rq.remoteDBID == 0, "remoteDBID cannot be used when existingRevision=false");
            }

            int                  commonAncestorIndex = 0;
            Retained<C4Document> doc;
            if ( rq.save && isNewDocPutRequest(rq) ) {
                // As an optimization, write the doc assuming there is no prior record in the db:
                std::tie(doc, commonAncestorIndex) = putNewDoc(rq);
                // If there's already a record, doc will be null, so we'll continue down regular path.
            }
            if ( !doc ) {
                if ( rq.existingRevision ) {
                    // Insert existing revision:
                    doc = getDocument(rq.docID, false, kDocGetAll);
                    C4Error err;
                    commonAncestorIndex = doc->putExistingRevision(rq, &err);
                    if ( commonAncestorIndex < 0 ) {
                        throwIfUnexpected(err, outError);
                        doc                 = nullptr;
                        commonAncestorIndex = 0;
                    }
                } else {
                    // Create new revision:
                    alloc_slice docID = (rq.docID.buf) ? alloc_slice(rq.docID) : C4Document::createDocID();

                    slice parentRevID;
                    if ( rq.historyCount > 0 ) parentRevID = rq.history[0];

                    doc = getDocument(docID, false, kDocGetAll);
                    C4Error err;
                    if ( !doc->checkNewRev(parentRevID, rq.revFlags, rq.allowConflict, &err)
                         || !doc->putNewRevision(rq, &err) ) {
                        throwIfUnexpected(err, outError);
                        doc = nullptr;
                    }
                    commonAncestorIndex = 0;
                }
            }

            Assert(commonAncestorIndex >= 0, "Unexpected conflict in c4doc_put");
            if ( outCommonAncestorIndex ) *outCommonAncestorIndex = commonAncestorIndex;
            return doc;
        }

        // Is this a PutRequest that doesn't require a Record to exist already?
        bool isNewDocPutRequest(const C4DocPutRequest& rq) const {
            if ( rq.deltaCB ) return false;
            else if ( rq.existingRevision )
                return documentFactory()->isFirstGenRevID(rq.history[rq.historyCount - 1]);
            else
                return rq.historyCount == 0;
        }

        // Tries to fulfil a PutRequest by creating a new Record. Returns null if one already exists.
        pair<Retained<C4Document>, int> putNewDoc(const C4DocPutRequest& rq) const {
            DebugAssert(rq.save, "putNewDoc optimization works only if rq.save is true");
            Record record(rq.docID);
            if ( !rq.docID.buf ) record.setKey(C4Document::createDocID());
            Retained<C4Document> doc = documentFactory()->newDocumentInstance(record);
            int                  commonAncestorIndex;
            if ( rq.existingRevision ) commonAncestorIndex = doc->putExistingRevision(rq, nullptr);
            else
                commonAncestorIndex = doc->putNewRevision(rq, nullptr) ? 0 : -1;
            if ( commonAncestorIndex < 0 ) doc = nullptr;
            return {doc, commonAncestorIndex};
        }

        void moveDocument(slice docID, C4Collection* toCollection, slice newDocID) override {
            C4Database::Transaction t(getDatabase());
            if ( newDocID ) C4Document::requireValidDocID(newDocID);
            keyStore().moveTo(docID, ((CollectionImpl*)toCollection)->keyStore(), dbImpl()->transaction(), newDocID);
            // DOES NOT NOTIFY SEQUENCE TRACKER! (should it?)
            t.commit();
        }

        void documentSaved(C4Document* doc) {
            // CBL-1089
            // Conflicted documents are not eligible to be replicated,
            // so ignore them.  Later when the conflict is resolved
            // there will be logic to replicate them (see TreeDocument::resolveConflict)
            if ( _sequenceTracker && !(doc->selectedRev().flags & kRevIsConflict) ) {
                Assert(doc->selectedRev().sequence == doc->sequence());  // The new revision must be selected
                auto st = _sequenceTracker->useLocked();
                st->documentChanged(doc->docID(),
                                    doc->getSelectedRevIDGlobalForm(),  // entire version vector
                                    doc->selectedRev().sequence, doc->getRevisionBody().size,
                                    SequenceTracker::RevisionFlags(doc->selectedRev().flags));
            }
        }

#pragma mark - PURGING / EXPIRING DOCS:

        C4Timestamp getExpiration(slice docID) const override { return keyStore().getExpiration(docID); }

        bool setExpiration(slice docID, C4Timestamp expiration) override {
            {
                C4Database::Transaction t(dbImpl());
                if ( !keyStore().setExpiration(docID, expiration) ) return false;
                t.commit();
            }

            if ( expiration > C4Timestamp::None ) {
                if ( _housekeeper ) _housekeeper->documentExpirationChanged(expiration);
                else
                    startHousekeeping();
            }
            return true;
        }

        bool purgeDocument(slice docID) override {
            C4Database::Transaction t(dbImpl());
            if ( !keyStore().del(docID, dbImpl()->transaction()) ) return false;
            if ( _sequenceTracker ) _sequenceTracker->useLocked()->documentPurged(docID);
            t.commit();
            return true;
        }

        C4Timestamp nextDocExpiration() const override { return keyStore().nextExpiration(); }

        int64_t purgeExpiredDocs() override {
            C4Database::Transaction t(getDatabase());
            int64_t                 count;
            if ( _sequenceTracker ) {
                auto st = _sequenceTracker->useLocked();
                count   = keyStore().expireRecords([&](slice docID) { st->documentPurged(docID); });
            } else {
                count = keyStore().expireRecords();
            }
            t.commit();
            return count;
        }

        void startHousekeeping() {
            if ( !_housekeeper && isValid() ) {
                if ( (getDatabase()->getConfiguration().flags & kC4DB_ReadOnly) == 0 ) {
                    _housekeeper = new Housekeeper(this);
                    _housekeeper->setParentObjectRef(getObjectRef());
                    _housekeeper->start();
                }
            }
        }

        bool stopHousekeeping() {
            if ( !_housekeeper ) return false;
            _housekeeper->stop();
            _housekeeper = nullptr;
            return true;
        }

#pragma mark - INDEXES:

        void createIndex(slice indexName, slice indexSpec, C4QueryLanguage indexLanguage, C4IndexType indexType,
                         const C4IndexOptions* indexOptions = nullptr) override {
            IndexSpec::Options options;
            switch ( indexType ) {
                case kC4ValueIndex:
                case kC4ArrayIndex:
                    break;
                case kC4FullTextIndex:
                    if ( indexOptions ) {
                        auto& ftsOpt            = options.emplace<IndexSpec::FTSOptions>();
                        ftsOpt.language         = indexOptions->language;
                        ftsOpt.ignoreDiacritics = indexOptions->ignoreDiacritics;
                        ftsOpt.disableStemming  = indexOptions->disableStemming;
                        ftsOpt.stopWords        = indexOptions->stopWords;
                    }
                    break;
#ifdef COUCHBASE_ENTERPRISE
                case kC4PredictiveIndex:
                    break;
                case kC4VectorIndex:
                    if ( indexOptions ) {
                        auto& c4Opt       = indexOptions->vector;
                        auto& vecOpt      = options.emplace<IndexSpec::VectorOptions>();
                        vecOpt.dimensions = c4Opt.dimensions;
                        switch ( c4Opt.metric ) {
                            case kC4VectorMetricEuclidean:
                                vecOpt.metric = vectorsearch::Metric::Euclidean2;
                                break;
                            case kC4VectorMetricCosine:
                                vecOpt.metric = vectorsearch::Metric::Cosine;
                                break;
                            case kC4VectorMetricDefault:
                                break;
                        }
                        switch ( c4Opt.clustering.type ) {
                            case kC4VectorClusteringFlat:
                                vecOpt.clustering = vectorsearch::FlatClustering{c4Opt.clustering.flat_centroids};
                                break;
                            case kC4VectorClusteringMulti:
                                vecOpt.clustering = vectorsearch::MultiIndexClustering{
                                        c4Opt.clustering.multi_subquantizers, c4Opt.clustering.multi_bits};
                                break;
                        }
                        switch ( c4Opt.encoding.type ) {
                            case kC4VectorEncodingNone:
                                vecOpt.encoding = vectorsearch::NoEncoding{};
                                break;
                            case kC4VectorEncodingPQ:
                                vecOpt.encoding =
                                        vectorsearch::PQEncoding{c4Opt.encoding.pq_subquantizers, c4Opt.encoding.bits};
                                break;
                            case kC4VectorEncodingSQ:
                                vecOpt.encoding = vectorsearch::SQEncoding{c4Opt.encoding.bits};
                                break;
                            case kC4VectorEncodingDefault:
                                break;
                        }
                        vecOpt.minTrainingCount = c4Opt.minTrainingSize;
                        vecOpt.maxTrainingCount = c4Opt.maxTrainingSize;
                        if ( c4Opt.numProbes > 0 ) vecOpt.probeCount = c4Opt.numProbes;
                        vecOpt.lazyEmbedding = c4Opt.lazy;
                        vecOpt.validate();
                    } else {
                        error::_throw(error::InvalidParameter, "Vector index requires options");
                    }
                    break;
#endif
                default:
                    error::_throw(error::InvalidParameter, "Invalid index type");
                    break;
            }
            keyStore().createIndex(indexName, indexSpec, (QueryLanguage)indexLanguage, (IndexSpec::Type)indexType,
                                   options);
        }

        Retained<C4Index> getIndex(slice name) override { return C4Index::getIndex(this, name); }

        void deleteIndex(slice indexName) override { keyStore().deleteIndex(indexName); }

        alloc_slice getIndexesInfo(bool fullInfo = true) const override {
            FLEncoder enc = FLEncoder_New();
            FLEncoder_BeginArray(enc, 2);
            for ( const auto& spec : keyStore().getIndexes() ) {
                if ( fullInfo ) {
                    FLEncoder_BeginDict(enc, 5);
                    FLEncoder_WriteKey(enc, slice("name"));
                    FLEncoder_WriteString(enc, slice(spec.name));
                    FLEncoder_WriteKey(enc, slice("type"));
                    FLEncoder_WriteInt(enc, spec.type);
                    FLEncoder_WriteKey(enc, slice("expr"));
                    FLEncoder_WriteString(enc, slice(spec.expression));
                    FLEncoder_WriteKey(enc, slice("lang"));
                    switch ( spec.queryLanguage ) {
                        case QueryLanguage::kJSON:
                            FLEncoder_WriteString(enc, slice("json"));
                            break;
                        case QueryLanguage::kN1QL:
                            FLEncoder_WriteString(enc, slice("n1ql"));
                            break;
                    }
                    if ( auto vecOpts = spec.vectorOptions() ) {
                        FLEncoder_WriteKey(enc, "vector_options"_sl);
                        FLEncoder_WriteString(enc, slice(vecOpts->createArgs()));
                    }
                    FLEncoder_EndDict(enc);
                } else {
                    FLEncoder_WriteString(enc, slice(spec.name));
                }
            }
            FLEncoder_EndArray(enc);
            alloc_slice ret{FLEncoder_Finish(enc, nullptr)};
            FLEncoder_Free(enc);
            return ret;
        }

        alloc_slice getIndexRows(slice indexName) const override {
            auto        dataFile = (SQLiteDataFile*)dbImpl()->dataFile();
            int64_t     rowCount;
            alloc_slice rows;
            dataFile->inspectIndex(indexName, rowCount, &rows);
            return rows;
        }

        bool isIndexTrained(slice indexName) const override { return keyStore().isIndexTrained(indexName); }

#pragma mark - OBSERVERS:

        std::unique_ptr<C4CollectionObserver> observe(CollectionObserverCallback cb) override {
            return C4CollectionObserver::create(this, cb);
        }

        std::unique_ptr<C4DocumentObserver> observeDocument(slice docID, DocumentObserverCallback cb) override {
            return C4DocumentObserver::create(this, docID, cb);
        }


      private:
        KeyStore*                                _keyStore;         // The actual DB table
        unique_ptr<DocumentFactory>              _documentFactory;  // creates C4Document instances
        unique_ptr<access_lock<SequenceTracker>> _sequenceTracker;  // Doc change tracker/notifier
        Retained<Housekeeper>                    _housekeeper;      // for expiration/cleanup tasks
    };

    static inline CollectionImpl* asInternal(C4Collection* coll) { return (CollectionImpl*)coll; }

    static inline const CollectionImpl* asInternal(const C4Collection* coll) { return (const CollectionImpl*)coll; }

}  // namespace litecore
