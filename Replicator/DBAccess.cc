//
// DBAccess.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "DBAccess.hh"
#include "DatabaseImpl.hh"
#include "ReplicatedRev.hh"
#include "ReplicatorTuning.hh"
#include "Error.hh"
#include "Stopwatch.hh"
#include "StringUtil.hh"
#include "c4BlobStore.hh"
#include "c4DocEnumerator.hh"
#include "c4Private.h"
#include <functional>
#include <set>
#include <utility>
#include <cinttypes>

namespace litecore::repl {

    using namespace std;
    using namespace fleece;

    DBAccess::DBAccess(C4Database* db, bool disableBlobSupport)
        : access_lock(db)
        , Logging(SyncLog)
        , _blobStore(&db->getBlobStore())
        , _disableBlobSupport(disableBlobSupport)
        , _revsToMarkSynced(bind(&DBAccess::markRevsSyncedNow, this), bind(&DBAccess::markRevsSyncedLater, this),
                            tuning::kInsertionDelay)
        , _timer([this] { markRevsSyncedNow(); })
        , _usingVersionVectors((db->getConfiguration().flags & kC4DB_VersionVectors) != 0) {}

    AccessLockedDB& DBAccess::insertionDB() {
        if ( !_insertionDB ) {
            useLocked([&](C4Database* db) {
                if ( !_insertionDB ) {
                    Retained<C4Database> idb;
                    try {
                        idb                = db->openAgain();
                        DatabaseImpl* impl = asInternal(idb);
                        logInfo("InsertionDB=%s", impl->dataFile()->loggingName().c_str());
                        _c4db_setDatabaseTag(idb, DatabaseTag_DBAccess);
                    } catch ( const exception& x ) {
                        C4Error error = C4Error::fromException(x);
                        logError("Couldn't open new db connection: %s", error.description().c_str());
                        idb = db;
                    }
                    _insertionDB.emplace(std::move(idb));
                }
            });
        }
        return *_insertionDB;
    }

    DBAccess::~DBAccess() { close(); }

    void DBAccess::close() {
        if ( _closed.test_and_set() ) { return; }
        _timer.stop();
        useLocked([this](Retained<C4Database>& db) {
            // Any use of the class after this will result in a crash that
            // should be easily identifiable, so forgo asserting if the pointer
            // is null in other areas.
            db            = nullptr;
            this->_sentry = &DBAccess::AssertDBOpen;
            if ( this->_insertionDB ) {
                this->_insertionDB->useLocked([](Retained<C4Database>& idb) { idb = nullptr; });
                this->_insertionDB.reset();
            }
        });
    }

    UseCollection DBAccess::useCollection(C4Collection* coll) { return {*this, coll}; }

    UseCollection DBAccess::useCollection(C4Collection* coll) const { return {*const_cast<DBAccess*>(this), coll}; }

    string DBAccess::convertVersionToAbsolute(slice revID) {
        string version(revID);
        if ( _usingVersionVectors ) {
            if ( _mySourceID.empty() ) {
                useLocked([&](C4Database* c4db) {
                    if ( _mySourceID.empty() ) _mySourceID = string(c4db->getSourceID());
                });
            }
            replace(version, "*", _mySourceID);
        }
        return version;
    }

    C4RemoteID DBAccess::lookUpRemoteDBID(slice key) {
        Assert(_remoteDBID == 0);
        _remoteDBID = useLocked()->getRemoteDBID(key, true);
        return _remoteDBID;
    }

    Retained<C4Document> DBAccess::getDoc(C4Collection* collection, slice docID, C4DocContentLevel content) const {
        return useCollection(collection)->getDocument(docID, true, content);
    }

    alloc_slice DBAccess::getDocRemoteAncestor(C4Document* doc) const {
        if ( _remoteDBID ) return doc->remoteAncestorRevID(_remoteDBID);
        else
            return {};
    }

    void DBAccess::setDocRemoteAncestor(C4Collection* coll, slice docID, slice revID) {
        if ( !_remoteDBID ) return;
        logInfo("Updating remote #%u's rev of '%.*s' to %.*s of collection %.*s.%.*s", _remoteDBID, SPLAT(docID),
                SPLAT(revID), SPLAT(coll->getSpec().scope), SPLAT(coll->getSpec().name));
        try {
            useLocked([&](C4Database* db) {
                Assert(db == coll->getDatabase());
                C4Database::Transaction t(db);
                Retained<C4Document>    doc = coll->getDocument(docID, true, kDocGetAll);
                if ( !doc ) error::_throw(error::NotFound);
                doc->setRemoteAncestorRevID(_remoteDBID, revID);
                doc->save();
                t.commit();
            });
        } catch ( const exception& x ) {
            C4Error error = C4Error::fromException(x);
            warn("Failed to update remote #%u's rev of '%.*s' to %.*s: %d/%d", _remoteDBID, SPLAT(docID), SPLAT(revID),
                 error.domain, error.code);
        }
    }

    unique_ptr<C4DocEnumerator> DBAccess::unresolvedDocsEnumerator(C4Collection* coll, bool orderByID) {
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        options.flags &= ~kC4IncludeBodies;
        options.flags &= ~kC4IncludeNonConflicted;
        options.flags |= kC4IncludeDeleted;
        if ( !orderByID ) options.flags |= kC4Unsorted;
        return useLocked<unique_ptr<C4DocEnumerator>>([&](const Retained<C4Database>& db) {
            DebugAssert(db.get() == coll->getDatabase());
            return make_unique<C4DocEnumerator>(coll, options);
        });
    }

    static bool containsAttachmentsProperty(slice json) {
        if ( !json.find(R"("_attachments":)") ) return false;
        Doc doc = Doc::fromJSON(json);
        return doc.root().asDict()[C4Blob::kLegacyAttachmentsProperty].asDict() != nullptr;
    }

    static inline bool isBlobOrAttachment(FLDeepIterator i, C4BlobKey* blobKey, bool noBlobs) {
        auto dict = FLValue_AsDict(FLDeepIterator_GetValue(i));
        if ( !dict ) return false;

        // Get the digest:
        if ( auto key = C4Blob::keyFromDigestProperty(dict); key ) *blobKey = *key;
        else
            return false;

        // Check if it's a blob:
        if ( !noBlobs && C4Blob::isBlob(dict) ) {
            return true;
        } else {
            // Check if it's an old-school attachment, i.e. in a top level "_attachments" dict:
            FLPathComponent* path;
            size_t           depth;
            FLDeepIterator_GetPath(i, &path, &depth);
            return depth == 2 && path[0].key == C4Blob::kLegacyAttachmentsProperty;
        }
    }

    void DBAccess::findBlobReferences(Dict root, bool unique, const FindBlobCallback& callback) const {
        // This method is non-static because it references _disableBlobSupport, but it's
        // thread-safe.
        set<string>    found;
        FLDeepIterator i = FLDeepIterator_New(root);
        for ( ; FLDeepIterator_GetValue(i); FLDeepIterator_Next(i) ) {
            C4BlobKey blobKey;
            if ( isBlobOrAttachment(i, &blobKey, _disableBlobSupport) ) {
                if ( !unique || found.emplace((const char*)&blobKey, sizeof(blobKey)).second ) {
                    auto blob = Value(FLDeepIterator_GetValue(i)).asDict();
                    callback(i, blob, blobKey);
                }
                FLDeepIterator_SkipChildren(i);
            }
        }
        FLDeepIterator_Free(i);
    }

    bool DBAccess::hasBlobReferences(Dict root) const {
        // This method is non-static because it references _disableBlobSupport, but it's
        // thread-safe.
        bool           found = false;
        FLDeepIterator i     = FLDeepIterator_New(root);
        for ( ; FLDeepIterator_GetValue(i); FLDeepIterator_Next(i) ) {
            C4BlobKey blobKey;
            if ( isBlobOrAttachment(i, &blobKey, _disableBlobSupport) ) {
                found = true;
                break;
            }
        }
        FLDeepIterator_Free(i);
        return found;
    }

    void DBAccess::encodeRevWithLegacyAttachments(fleece::Encoder& enc, Dict root, unsigned revpos) const {
        enc.beginDict();

        // Write existing properties except for _attachments:
        Dict oldAttachments;
        for ( Dict::iterator i(root); i; ++i ) {
            slice key = i.keyString();
            if ( key == C4Blob::kLegacyAttachmentsProperty ) {
                oldAttachments = i.value().asDict();  // remember _attachments dict for later
            } else {
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Now write _attachments:
        enc.writeKey(C4Blob::kLegacyAttachmentsProperty);
        enc.beginDict();
        // First pre-existing legacy attachments, if any:
        for ( Dict::iterator i(oldAttachments); i; ++i ) {
            slice key = i.keyString();
            if ( !key.hasPrefix("blob_"_sl) ) {
                // TODO: Should skip this entry if a blob with the same digest exists
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Then entries for blobs found in the document:
        findBlobReferences(root, false, [&](FLDeepIterator di, FLDict blob, C4BlobKey blobKey) {
            alloc_slice path(FLDeepIterator_GetJSONPointer(di));
            if ( path.hasPrefix("/_attachments/"_sl) ) return;
            string attName = string("blob_") + string(path);
            enc.writeKey(slice(attName));
            enc.beginDict();
            for ( Dict::iterator i(blob); i; ++i ) {
                slice key = i.keyString();
                if ( key != C4Document::kObjectTypeProperty && key != "stub"_sl ) {
                    enc.writeKey(key);
                    enc.writeValue(i.value());
                }
            }
            enc.writeKey("stub"_sl);
            enc.writeBool(true);
            if ( revpos > 0 ) {
                enc.writeKey("revpos"_sl);
                enc.writeInt(revpos);
            }
            enc.endDict();
        });
        enc.endDict();

        enc.endDict();
    }

    SharedKeys DBAccess::tempSharedKeys() {
        SharedKeys sk;
        {
            lock_guard<mutex> lock(_tempSharedKeysMutex);
            sk = _tempSharedKeys;
        }
        if ( !sk ) sk = updateTempSharedKeys();
        return sk;
    }

    SharedKeys DBAccess::updateTempSharedKeys() {
        auto&      db = _insertionDB ? *_insertionDB : *this;
        SharedKeys result;
        return db.useLocked<SharedKeys>([&](C4Database* idb) {
            SharedKeys        dbsk = idb->getFleeceSharedKeys();
            lock_guard<mutex> lock(_tempSharedKeysMutex);
            if ( !_tempSharedKeys || _tempSharedKeysInitialCount < dbsk.count() ) {
                // Copy database's sharedKeys:
                _tempSharedKeys             = SharedKeys::create(dbsk.stateData());
                _tempSharedKeysInitialCount = dbsk.count();
                int retryCount              = 0;
                while ( _usuallyFalse(_tempSharedKeys.count() != dbsk.count() && retryCount++ < 10) ) {
                    // CBL-4288: Possible compiler optimization issue?  If these two counts
                    // are not equal then the shared keys creation process has been corrupted
                    // and we must not continue as-is because then we will have data corruption

                    // This really should not be the solution, but yet it reliably seems to stop
                    // this weirdness from happening
                    Warn("CBL-4288: Shared keys creation process failed, retrying...");
                    _tempSharedKeys = SharedKeys::create(dbsk.stateData());
                }

                if ( _usuallyFalse(_tempSharedKeys.count() != dbsk.count()) ) {
                    // The above loop failed, so force an error condition to prevent a bad write
                    // Note: I have never seen this happen, it is here just because the alternative
                    // is data corruption, which is absolutely unacceptable
                    WarnError("CBL-4288: Retrying 10 times did not solve the issue, aborting document encode...");
                    _tempSharedKeys = SharedKeys();
                }

                assert(_tempSharedKeys);
            }
            _tempSharedKeys.disableCaching();
            return _tempSharedKeys;
        });
    }

    Doc DBAccess::tempEncodeJSON(slice jsonBody, FLError* err) {
        Encoder enc;
        auto    tsk = tempSharedKeys();
        if ( !tsk ) {
            // Error logged in updateTempSharedKeys
            if ( err ) *err = kFLInternalError;
            return {};
        }

        enc.setSharedKeys(tsk);
        if ( !enc.convertJSON(jsonBody) ) {
            *err = enc.error();
            WarnError("Fleece encoder convertJSON failed (%d)", *err);
            return {};
        }

        Doc doc = enc.finishDoc();
        if ( !doc && err ) {
            WarnError("Fleece encoder finishDoc failed (%d)", *err);
            *err = enc.error();
        }

        return doc;
    }

    alloc_slice DBAccess::reEncodeForDatabase(Doc doc) {
        bool reEncode;
        {
            lock_guard<mutex> lock(_tempSharedKeysMutex);
            reEncode = doc.sharedKeys() != (FLSharedKeys)_tempSharedKeys
                       || _tempSharedKeys.count() > _tempSharedKeysInitialCount;
        }
        if ( reEncode ) {
            // Re-encode with database's current sharedKeys:
            // insertionDB() asserts DB open, no need to do it here
            return insertionDB().useLocked<alloc_slice>([&](C4Database* idb) {
                SharedEncoder enc(idb->sharedFleeceEncoder());
                enc.writeValue(doc.root());
                alloc_slice data = enc.finish();
                enc.reset();
                return data;
            });
        } else {
            // _tempSharedKeys is still compatible with database's sharedKeys, so no re-encoding.
            // But we do need to copy the data, because the data in doc is tagged with the temp
            // sharedKeys, and the database needs to tag the inserted data with its own.
            return alloc_slice(doc.data());
        }
    }

    Doc DBAccess::applyDelta(C4Document* doc, slice deltaJSON, bool useDBSharedKeys) {
        Dict srcRoot = doc->getProperties();
        if ( !srcRoot )
            error::_throw(error::CorruptRevisionData, "DBAccess applyDelta error getting document's properties");

        bool useLegacyAttachments = !_disableBlobSupport && containsAttachmentsProperty(deltaJSON);
        Doc  reEncodedDoc;
        if ( useLegacyAttachments || !useDBSharedKeys ) {
            Encoder enc;
            enc.setSharedKeys(tempSharedKeys());
            if ( useLegacyAttachments ) {
                // Delta refers to legacy attachments, so convert my base revision to have them:
                encodeRevWithLegacyAttachments(enc, srcRoot, 1);
            } else {
                // Can't use DB SharedKeys, so re-encode to temp encoder
                enc.writeValue(srcRoot);
            }
            reEncodedDoc = enc.finishDoc();
            srcRoot      = reEncodedDoc.root().asDict();
        }

        Doc     result;
        FLError flErr;
#ifdef LITECORE_CPPTEST
        slice cbl_4499_errDoc = "cbl-4499_doc-001"_sl;
        if ( doc->docID().hasSuffix(cbl_4499_errDoc) ) {
            flErr = kFLInvalidData;
        } else {
#endif
            if ( useDBSharedKeys ) {
                // insertionDB() asserts DB open, no need to do it here
                insertionDB().useLocked([&](C4Database* idb) {
                    SharedEncoder enc(idb->sharedFleeceEncoder());
                    JSONDelta::apply(srcRoot, deltaJSON, enc);
                    result = enc.finishDoc(&flErr);
                });
            } else {
                Encoder enc;
                enc.setSharedKeys(tempSharedKeys());
                JSONDelta::apply(srcRoot, deltaJSON, enc);
                result = enc.finishDoc(&flErr);
            }
#ifdef LITECORE_CPPTEST
        }
#endif
        ++gNumDeltasApplied;

        if ( !result ) {
            if ( flErr == kFLInvalidData ) error::_throw(error::CorruptDelta, "Invalid delta");
            else
                error::_throw(error::Fleece, flErr);
        }
        return result;
    }

    Doc DBAccess::applyDelta(C4Collection* collection, slice docID, slice baseRevID, slice deltaJSON) {
        Retained<C4Document> doc = getDoc(collection, docID, kDocGetUpgraded);
        if ( !doc ) error::_throw(error::NotFound);
        if ( !doc->selectRevision(baseRevID, true) || !doc->loadRevisionBody() ) return nullptr;
        return applyDelta(doc, deltaJSON, false);
    }

    void DBAccess::markRevSynced(ReplicatedRev* rev NONNULL) { _revsToMarkSynced.push(rev); }

    // Mark all the queued revisions as synced to the server.
    void DBAccess::markRevsSyncedNow() {
        _timer.stop();
        auto revs = _revsToMarkSynced.pop();
        if ( !revs ) return;

        Stopwatch st;
        // insertionDB() asserts DB open, no need to do it here
        insertionDB().useLocked([&](C4Database* idb) {
            try {
                C4Database::Transaction transaction(idb);
                for ( ReplicatedRev* rev : *revs ) {
                    C4CollectionSpec coll       = rev->collectionSpec;
                    C4Collection*    collection = idb->getCollection(coll);
                    if ( collection == nullptr ) {
                        C4Error::raise(LiteCoreDomain, kC4ErrorNotOpen, "%s",
                                       stringprintf("Failed to find collection '%*s.%*s'.", SPLAT(coll.scope),
                                                    SPLAT(coll.name))
                                               .c_str());
                    }
                    logDebug("Marking rev '%.*s'.%.*s '%.*s' %.*s (#%" PRIu64 ") as synced to remote db %u",
                             SPLAT(coll.scope), SPLAT(coll.name), SPLAT(rev->docID), SPLAT(rev->revID),
                             static_cast<uint64_t>(rev->sequence), remoteDBID());
                    try {
                        collection->markDocumentSynced(rev->docID, rev->revID, rev->sequence,
                                                       rev->rejectedByRemote ? 0 : remoteDBID());
                    } catch ( const exception& x ) {
                        C4Error error = C4Error::fromException(x);
                        warn("Unable to mark '%.*s'.%.*s '%.*s' %.*s (#%" PRIu64 ") as synced; error %d/%d",
                             SPLAT(coll.scope), SPLAT(coll.name), SPLAT(rev->docID), SPLAT(rev->revID),
                             (uint64_t)rev->sequence, error.domain, error.code);
                    }
                }
                transaction.commit();
                double t = st.elapsed();
                logVerbose("Marked %zu revs as synced-to-server in %.2fms (%.0f/sec)", revs->size(), t * 1000,
                           (double)revs->size() / t);
            } catch ( const exception& x ) {
                C4Error error = C4Error::fromException(x);
                warn("Error marking %zu revs as synced: %d/%d", revs->size(), error.domain, error.code);
            }
        });
    }

    void DBAccess::markRevsSyncedLater() { _timer.fireAfter(tuning::kInsertionDelay); }

    atomic<unsigned> DBAccess::gNumDeltasApplied;


}  // namespace litecore::repl
