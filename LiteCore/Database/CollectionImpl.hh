//
// CollectionImpl.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
//
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

#include "c4Collection.hh"
#include "c4BlobStore.hh"
#include "c4Database.hh"
#include "c4Document.hh"
#include "c4ExceptionUtils.hh"
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
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.h"
#include "betterassert.hh"


namespace litecore {

    class CollectionImpl final : public C4Collection, public Logging {
    public:
        CollectionImpl(C4Database *db, slice name, KeyStore &store)
        :C4Collection(db, name)
        ,Logging(DBLog)
        ,_keyStore(&store)
        {
            auto flags = _database->getConfiguration().flags;
            if (flags & kC4DB_VersionVectors)
                _documentFactory = std::make_unique<VectorDocumentFactory>(this);
            else
                _documentFactory = std::make_unique<TreeDocumentFactory>(this);

            if (!(_database->getConfiguration().flags & kC4DB_NonObservable))
                _sequenceTracker = std::make_unique<access_lock<SequenceTracker>>(
                                                                    SequenceTracker(store.name()));

            logInfo("Instantiated");
        }


        ~CollectionImpl() {
            destructExtraInfo(_extraInfo);
        }


        void close() {
            logInfo("Closed");
            _housekeeper = nullptr;
            _sequenceTracker = nullptr;
            _documentFactory = nullptr;
            _keyStore = nullptr;
            _database = nullptr;
        }


        std::string loggingIdentifier() const override {  // Logging API
            auto dbName = _database->getName();
            return format("%.*s/%.*s", SPLAT(dbName), SPLAT(_name));
        }


        uint64_t getDocumentCount() const override  {return keyStore().recordCount();}
        sequence_t getLastSequence() const override {return keyStore().lastSequence();}
        KeyStore& keyStore() const         {precondition(_keyStore); return *_keyStore;}

        DatabaseImpl* dbImpl()                      {return asInternal(getDatabase());}
        const DatabaseImpl* dbImpl() const          {return asInternal(getDatabase());}


        access_lock<SequenceTracker>& sequenceTracker() {
            if (!_sequenceTracker)
                error::_throw(error::UnsupportedOperation);
            return *_sequenceTracker;
        }


#pragma mark - TRANSACTIONS:


        void transactionBegan() {
            if (_sequenceTracker)
                _sequenceTracker->useLocked()->beginTransaction();
        }


        bool changedDuringTransaction() {
            return _sequenceTracker && _sequenceTracker->useLocked()->changedDuringTransaction();
        }


        void transactionEnding(ExclusiveTransaction *transaction, bool committing) {
            if (_sequenceTracker) {
                auto st = _sequenceTracker->useLocked();
                // Notify other Database instances on this file:
                if (committing && st->changedDuringTransaction())
                    transaction->notifyCommitted(st);
                st->endTransaction(committing);
            }
        }


        void externalTransactionCommitted(const SequenceTracker &sourceTracker) {
            if (_sequenceTracker)
                _sequenceTracker->useLocked()->addExternalTransaction(sourceTracker);
        }


#pragma mark - BLOBS:


        void findBlobReferences(const fleece::function_ref<bool(FLDict)> &blobCallback) override {
            uint64_t numRevisions = 0;
            RecordEnumerator::Options options;
            options.onlyBlobs = true;
            options.sortOption = kUnsorted;
            RecordEnumerator e(keyStore(), options);
            while (e.next()) {
                Retained<C4Document> doc = _documentFactory->newDocumentInstance(*e);
                doc->selectCurrentRevision();
                do {
                    if (doc->loadRevisionBody()) {
                        FLDict body = doc->getProperties();
                        C4Blob::findBlobReferences(body, blobCallback);
                        C4Blob::findAttachmentReferences(body, blobCallback);
                        ++numRevisions;
                    }
                } while(doc->selectNextRevision());
            }
        }


#pragma mark - DOCUMENTS:


        virtual Retained<C4Document> newDocumentInstance(const litecore::Record &record) {
            return _documentFactory->newDocumentInstance(record);
        }


        Retained<C4Document> getDocument(slice docID,
                                         bool mustExist,
                                         C4DocContentLevel content) const override
        {
            auto doc = _documentFactory->newDocumentInstance(docID, ContentOption(content));
            if (mustExist && !doc->exists())
                doc = nullptr;
            return doc;
        }


        Retained<C4Document> getDocumentBySequence(C4SequenceNumber sequence) const override {
            if (Record rec = keyStore().get(sequence, kEntireBody); rec.exists())
                return _documentFactory->newDocumentInstance(move(rec));
            else
                return nullptr;
        }


        std::vector<alloc_slice> findDocAncestors(const std::vector<slice> &docIDs,
                                                  const std::vector<slice> &revIDs,
                                                  unsigned maxAncestors,
                                                  bool mustHaveBodies,
                                                  C4RemoteID remoteDBID) const override
        {
            return _documentFactory->findAncestors(docIDs, revIDs, maxAncestors,
                                                   mustHaveBodies, remoteDBID);
        }


        // Errors other than NotFound, Conflict and delta failures
        // should be thrown as exceptions, in the C++ API.
        static void throwIfUnexpected(const C4Error &inError, C4Error *outError) {
            if (outError)
                *outError = inError;
            if (inError.domain == LiteCoreDomain) {
                switch (inError.code) {
                    case kC4ErrorNotFound:
                    case kC4ErrorConflict:
                    case kC4ErrorDeltaBaseUnknown:
                    case kC4ErrorCorruptDelta:
                        return; // don't throw these errors
                }
            }
            C4Error::raise(inError);
        }


        bool markDocumentSynced(slice docID,
                                slice revID,
                                C4SequenceNumber sequence,
                                C4RemoteID remoteID) override
        {
            if (remoteID == RevTree::kDefaultRemoteID) {
                // Shortcut: can set kSynced flag on the record to mark that the current revision is
                // synced to remote #1. But the call will return false if the sequence no longer
                // matches, i.e this revision is no longer current. Then have to take the slow approach.
                if (keyStore().setDocumentFlag(docID, sequence,
                                           DocumentFlags::kSynced,
                                           dbImpl()->transaction())) {
                    return true;
                }
            }

            // Slow path: Load the doc and update the remote-ancestor info in the rev tree:
            Retained<C4Document> doc = getDocument(docID, true, kDocGetAll);
            if (!doc)
                return false;
            if (!revID) {
                // Look up revID by sequence, if it wasn't given:
                Assert(sequence != 0);
                do {
                    if (doc->selectedRev().sequence == sequence) {
                        revID = doc->selectedRev().revID;
                        break;
                    }
                } while (doc->selectNextRevision());
                if (!revID)
                    return false;
            }
            doc->setRemoteAncestorRevID(remoteID, revID);
            doc->save();
            return true;
        }


        Retained<C4Document> createDocument(slice docID,
                                            slice revBody,
                                            C4RevisionFlags revFlags,
                                            C4Error *outError) override
        {
            C4DocPutRequest rq = {};
            rq.docID = docID;
            rq.body = revBody;
            rq.revFlags = revFlags;
            rq.save = true;
            return putDocument(rq, nullptr, outError);
        }


        Retained<C4Document> putDocument(const C4DocPutRequest &rq,
                                         size_t *outCommonAncestorIndex,
                                         C4Error *outError) override
        {
            dbImpl()->mustBeInTransaction();
            if (rq.docID.buf && !C4Document::isValidDocID(rq.docID))
                error::_throw(error::BadDocID);
            if (rq.existingRevision || rq.historyCount > 0)
                AssertParam(rq.docID.buf, "Missing docID");
            if (rq.existingRevision) {
                AssertParam(rq.historyCount > 0, "No history");
            } else {
                AssertParam(rq.historyCount <= 1, "Too much history");
                AssertParam(rq.historyCount > 0 || !(rq.revFlags & kRevDeleted),
                            "Can't create a new already-deleted document");
                AssertParam(rq.remoteDBID == 0, "remoteDBID cannot be used when existingRevision=false");
            }

            int commonAncestorIndex = 0;
            Retained<C4Document> doc;
            if (rq.save && isNewDocPutRequest(rq)) {
                // As an optimization, write the doc assuming there is no prior record in the db:
                std::tie(doc, commonAncestorIndex) = putNewDoc(rq);
                // If there's already a record, doc will be null, so we'll continue down regular path.
            }
            if (!doc) {
                if (rq.existingRevision) {
                    // Insert existing revision:
                    doc = getDocument(rq.docID, false, kDocGetAll);
                    C4Error err;
                    commonAncestorIndex = doc->putExistingRevision(rq, &err);
                    if (commonAncestorIndex < 0) {
                        throwIfUnexpected(err, outError);
                        doc = nullptr;
                        commonAncestorIndex = 0;
                    }
                } else {
                    // Create new revision:
                    slice docID = rq.docID;
                    alloc_slice newDocID;
                    if (!docID)
                        docID = newDocID = C4Document::createDocID();

                    slice parentRevID;
                    if (rq.historyCount > 0)
                        parentRevID = rq.history[0];

                    doc = getDocument(docID, false, kDocGetAll);
                    C4Error err;
                    if (!doc->checkNewRev(parentRevID, rq.revFlags, rq.allowConflict, &err)
                        || !doc->putNewRevision(rq, &err)) {
                        throwIfUnexpected(err, outError);
                        doc = nullptr;
                    }
                    commonAncestorIndex = 0;
                }
            }

            Assert(commonAncestorIndex >= 0, "Unexpected conflict in c4doc_put");
            if (outCommonAncestorIndex)
                *outCommonAncestorIndex = commonAncestorIndex;
            return doc;
        }


        // Is this a PutRequest that doesn't require a Record to exist already?
        bool isNewDocPutRequest(const C4DocPutRequest &rq) {
            if (rq.deltaCB)
                return false;
            else if (rq.existingRevision)
                return _documentFactory->isFirstGenRevID(rq.history[rq.historyCount-1]);
            else
                return rq.historyCount == 0;
        }


        // Tries to fulfil a PutRequest by creating a new Record. Returns null if one already exists.
        pair<Retained<C4Document>,int> putNewDoc(const C4DocPutRequest &rq) {
            DebugAssert(rq.save, "putNewDoc optimization works only if rq.save is true");
            Record record(rq.docID);
            if (!rq.docID.buf)
                record.setKey(C4Document::createDocID());
            Retained<C4Document> doc = _documentFactory->newDocumentInstance(record);
            int commonAncestorIndex;
            if (rq.existingRevision)
                commonAncestorIndex = doc->putExistingRevision(rq, nullptr);
            else
                commonAncestorIndex = doc->putNewRevision(rq, nullptr) ? 0 : -1;
            if (commonAncestorIndex < 0)
                doc = nullptr;
            return {doc, commonAncestorIndex};
        }


        void moveDocument(slice docID, C4Collection *toCollection, slice newDocID) override {
            C4Database::Transaction t(getDatabase());
            if (newDocID)
                C4Document::requireValidDocID(newDocID);
            keyStore().moveTo(docID, ((CollectionImpl*)toCollection)->keyStore(),
                              dbImpl()->transaction(), newDocID);
            // DOES NOT NOTIFY SEQUENCE TRACKER! (should it?)
            t.commit();
        }


        void documentSaved(C4Document* doc) {
            // CBL-1089
            // Conflicted documents are not eligible to be replicated,
            // so ignore them.  Later when the conflict is resolved
            // there will be logic to replicate them (see TreeDocument::resolveConflict)
            if (_sequenceTracker && !(doc->selectedRev().flags & kRevIsConflict)) {
                Assert(doc->selectedRev().sequence == doc->sequence()); // The new revision must be selected
                auto st = _sequenceTracker->useLocked();
                st->documentChanged(doc->docID(),
                                    doc->getSelectedRevIDGlobalForm(), // entire version vector
                                    doc->selectedRev().sequence,
                                    SequenceTracker::RevisionFlags(doc->selectedRev().flags));
            }
        }


#pragma mark - PURGING / EXPIRING DOCS:


        C4Timestamp getExpiration(slice docID) const override {
            return keyStore().getExpiration(docID);
        }


        bool setExpiration(slice docID, expiration_t expiration) override {
            {
                C4Database::Transaction t(dbImpl());
                if (!keyStore().setExpiration(docID, expiration))
                    return false;
                t.commit();
            }

            if (expiration > 0) {
                if (_housekeeper)
                    _housekeeper->documentExpirationChanged(expiration);
                else
                    startHousekeeping();
            }
            return true;
        }


        bool purgeDocument(slice docID) override {
            C4Database::Transaction t(dbImpl());
            if (!keyStore().del(docID, dbImpl()->transaction()))
                return false;
            if (_sequenceTracker)
                _sequenceTracker->useLocked()->documentPurged(docID);
            t.commit();
            return true;
        }


        C4Timestamp nextDocExpiration() const override {
            return keyStore().nextExpiration();
        }


        int64_t purgeExpiredDocs() override {
            C4Database::Transaction t(getDatabase());
            int64_t count;
            if (_sequenceTracker) {
                auto st = _sequenceTracker->useLocked();
                count = keyStore().expireRecords([&](slice docID) {
                    st->documentPurged(docID);
                });
            } else {
                count = keyStore().expireRecords();
            }
            t.commit();
            return count;
        }


        void startHousekeeping() {
            if (!_housekeeper) {
                if ((getDatabase()->getConfiguration().flags & kC4DB_ReadOnly) == 0) {
                    _housekeeper = new Housekeeper(this);
                    _housekeeper->start();
                }
            }
        }


        bool stopHousekeeping() {
            if (!_housekeeper)
                return false;
            _housekeeper->stop();
            _housekeeper = nullptr;
            return true;
        }


#pragma mark - INDEXES:


        static_assert(sizeof(C4IndexOptions) == sizeof(IndexSpec::Options));


        void createIndex(slice indexName,
                         slice indexSpecJSON,
                         C4IndexType indexType,
                         const C4IndexOptions* indexOptions =nullptr) override
        {
            keyStore().createIndex(indexName,
                               indexSpecJSON,
                               (IndexSpec::Type)indexType,
                               (const IndexSpec::Options*)indexOptions);
        }

        void deleteIndex(slice indexName) override {
            keyStore().deleteIndex(indexName);
        }

        alloc_slice getIndexesInfo(bool fullInfo = true) const override {
            FLEncoder enc = FLEncoder_New();
            FLEncoder_BeginArray(enc, 2);
            for (const auto &spec : keyStore().getIndexes()) {
                if (fullInfo) {
                    FLEncoder_BeginDict(enc, 3);
                    FLEncoder_WriteKey(enc, slice("name"));
                    FLEncoder_WriteString(enc, slice(spec.name));
                    FLEncoder_WriteKey(enc, slice("type"));
                    FLEncoder_WriteInt(enc, spec.type);
                    FLEncoder_WriteKey(enc, slice("expr"));
                    FLEncoder_WriteString(enc, slice(spec.expressionJSON));
                    FLEncoder_EndDict(enc);
                } else {
                    FLEncoder_WriteString(enc, slice(spec.name));
                }
            }
            FLEncoder_EndArray(enc);
            return alloc_slice(FLEncoder_Finish(enc, nullptr));
        }

        alloc_slice getIndexRows(slice indexName) const override {
            auto dataFile = (SQLiteDataFile*)dbImpl()->dataFile();
            int64_t rowCount;
            alloc_slice rows;
            dataFile->inspectIndex(indexName, rowCount, &rows);
            return rows;
        }


#pragma mark - OBSERVERS:


        virtual std::unique_ptr<C4CollectionObserver> observe(CollectionObserverCallback cb) override {
            return C4CollectionObserver::create(this, cb);
        }

        virtual std::unique_ptr<C4DocumentObserver> observeDocument(slice docID,
                                                                    DocumentObserverCallback cb) override {
            return C4DocumentObserver::create(this, docID, cb);
        }


    private:
        KeyStore* _keyStore;
        unique_ptr<DocumentFactory> _documentFactory;
        unique_ptr<access_lock<SequenceTracker>> _sequenceTracker; // Doc change tracker/notifier
        Retained<Housekeeper>       _housekeeper;           // for expiration/cleanup tasks
    };


    static inline CollectionImpl* asInternal(C4Collection *coll) {return (CollectionImpl*)coll;}

}
