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
#include "ReplicatedRev.hh"
#include "ReplicatorTuning.hh"
#include "Error.hh"
#include "Stopwatch.hh"
#include "StringUtil.hh"
#include "c4BlobStore.hh"
#include "c4Document.hh"
#include "c4DocEnumerator.hh"
#include "c4Private.h"
#include <functional>
#include <set>
#include <utility>
#include <cinttypes>

namespace litecore { namespace repl {

    using namespace std;
    using namespace fleece;


    DBAccess::DBAccess(C4Database* db, bool disableBlobSupport)
    :access_lock(move(db))
    ,Logging(SyncLog)
    ,_blobStore(&db->getBlobStore())
    ,_disableBlobSupport(disableBlobSupport)
    ,_revsToMarkSynced(bind(&DBAccess::markRevsSyncedNow, this),
                       bind(&DBAccess::markRevsSyncedLater, this),
                       tuning::kInsertionDelay)
    ,_timer(bind(&DBAccess::markRevsSyncedNow, this))
    ,_usingVersionVectors((db->getConfiguration().flags & kC4DB_VersionVectors) != 0)
    {
        // Copy database's sharedKeys:
        SharedKeys dbsk = db->getFleeceSharedKeys();
    }


    AccessLockedDB& DBAccess::insertionDB() {
        if (!_insertionDB) {
            useLocked([&](C4Database *db) {
                if (!_insertionDB) {
                    Retained<C4Database> idb;
                    try {
                        idb = db->openAgain();
                        _c4db_setDatabaseTag(idb, DatabaseTag_DBAccess);
                    } catch (const exception &x) {
                        C4Error error = C4Error::fromException(x);
                        logError("Couldn't open new db connection: %s", error.description().c_str());
                        idb = db;
                    }
                    _insertionDB.emplace(move(idb));
                }
            });
        }
        return *_insertionDB;
    }


    DBAccess::~DBAccess() {
        close();
    }


    void DBAccess::close() {
        if (_closed.test_and_set()) {
            return;
        }
        _timer.stop();
        useLocked([this](Retained<C4Database>& db) {
            // Any use of the class after this will result in a crash that
            // should be easily identifiable, so forgo asserting if the pointer
            // is null in other areas.
            db = nullptr;
            this->_sentry = &DBAccess::AssertDBOpen;
            if (this->_insertionDB) {
                this->_insertionDB->useLocked([](Retained<C4Database>& idb) {
                    idb = nullptr;
                });
                this->_insertionDB.reset();
            }
        });
    }


    string DBAccess::convertVersionToAbsolute(slice revID) {
        string version(revID);
        if (_usingVersionVectors) {
            if (_myPeerID.empty()) {
                useLocked([&](C4Database *c4db) {
                    if (_myPeerID.empty())
                        _myPeerID = string(c4db->getPeerID());
                });
            }
            replace(version, "*", _myPeerID);
        }
        return version;
    }


    C4RemoteID DBAccess::lookUpRemoteDBID(slice key) {
        Assert(_remoteDBID == 0);
        _remoteDBID = useLocked()->getRemoteDBID(key, true);
        return _remoteDBID;
    }


    alloc_slice DBAccess::getDocRemoteAncestor(C4Document *doc) {
        if (_remoteDBID)
            return doc->remoteAncestorRevID(_remoteDBID);
        else
            return {};
    }
    
    void DBAccess::setDocRemoteAncestor(slice docID, slice revID) {
        if (!_remoteDBID)
            return;
        logInfo("Updating remote #%u's rev of '%.*s' to %.*s",
                _remoteDBID, SPLAT(docID), SPLAT(revID));
        try {
            useLocked([&](C4Database *db) {
                C4Database::Transaction t(db);
                Retained<C4Document> doc = db->getDocument(docID, true, kDocGetAll);
                if (!doc)
                    error::_throw(error::NotFound);
                doc->setRemoteAncestorRevID(_remoteDBID, revID);
                doc->save();
                t.commit();
            });
        } catch (const exception &x) {
            C4Error error = C4Error::fromException(x);
            warn("Failed to update remote #%u's rev of '%.*s' to %.*s: %d/%d",
                 _remoteDBID, SPLAT(docID), SPLAT(revID), error.domain, error.code);
        }
    }

    unique_ptr<C4DocEnumerator> DBAccess::unresolvedDocsEnumerator(bool orderByID) {
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        options.flags &= ~kC4IncludeBodies;
        options.flags &= ~kC4IncludeNonConflicted;
        options.flags |= kC4IncludeDeleted;
        if (!orderByID)
            options.flags |= kC4Unsorted;
        return make_unique<C4DocEnumerator>(useLocked(), options);
    }


    static bool containsAttachmentsProperty(slice json) {
        if (!json.find("\"_attachments\":"_sl))
            return false;
        Doc doc = Doc::fromJSON(json);
        return doc.root().asDict()[C4Blob::kLegacyAttachmentsProperty].asDict() != nullptr;
    }


    static inline bool isBlobOrAttachment(FLDeepIterator i, C4BlobKey *blobKey, bool noBlobs) {
        auto dict = FLValue_AsDict(FLDeepIterator_GetValue(i));
        if (!dict)
            return false;

        // Get the digest:
        if (auto key = C4Blob::keyFromDigestProperty(dict); key)
            *blobKey = *key;
        else
            return false;

        // Check if it's a blob:
        if (!noBlobs && C4Blob::isBlob(dict)) {
            return true;
        } else {
            // Check if it's an old-school attachment, i.e. in a top level "_attachments" dict:
            FLPathComponent* path;
            size_t depth;
            FLDeepIterator_GetPath(i, &path, &depth);
            return depth == 2 && path[0].key == C4Blob::kLegacyAttachmentsProperty;
        }
    }


    void DBAccess::findBlobReferences(Dict root, bool unique, const FindBlobCallback &callback) {
        // This method is non-static because it references _disableBlobSupport, but it's
        // thread-safe.
        set<string> found;
        FLDeepIterator i = FLDeepIterator_New(root);
        for (; FLDeepIterator_GetValue(i); FLDeepIterator_Next(i)) {
            C4BlobKey blobKey;
            if (isBlobOrAttachment(i, &blobKey, _disableBlobSupport)) {
                if (!unique || found.emplace((const char*)&blobKey, sizeof(blobKey)).second) {
                    auto blob = Value(FLDeepIterator_GetValue(i)).asDict();
                    callback(i, blob, blobKey);
                }
                FLDeepIterator_SkipChildren(i);
            }
        }
        FLDeepIterator_Free(i);
    }


    void DBAccess::encodeRevWithLegacyAttachments(fleece::Encoder& enc, Dict root,
                                                 unsigned revpos)
    {
        enc.beginDict();

        // Write existing properties except for _attachments:
        Dict oldAttachments;
        for (Dict::iterator i(root); i; ++i) {
            slice key = i.keyString();
            if (key == C4Blob::kLegacyAttachmentsProperty) {
                oldAttachments = i.value().asDict();    // remember _attachments dict for later
            } else {
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Now write _attachments:
        enc.writeKey(C4Blob::kLegacyAttachmentsProperty);
        enc.beginDict();
        // First pre-existing legacy attachments, if any:
        for (Dict::iterator i(oldAttachments); i; ++i) {
            slice key = i.keyString();
            if (!key.hasPrefix("blob_"_sl)) {
                // TODO: Should skip this entry if a blob with the same digest exists
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Then entries for blobs found in the document:
        findBlobReferences(root, false, [&](FLDeepIterator di, FLDict blob, C4BlobKey blobKey) {
            alloc_slice path(FLDeepIterator_GetJSONPointer(di));
            if (path.hasPrefix("/_attachments/"_sl))
                return;
            string attName = string("blob_") + string(path);
            enc.writeKey(slice(attName));
            enc.beginDict();
            for (Dict::iterator i(blob); i; ++i) {
                slice key = i.keyString();
                if (key != C4Document::kObjectTypeProperty && key != "stub"_sl) {
                    enc.writeKey(key);
                    enc.writeValue(i.value());
                }
            }
            enc.writeKey("stub"_sl);
            enc.writeBool(true);
            enc.writeKey("revpos"_sl);
            enc.writeInt(revpos);
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
        if (!sk)
            sk = updateTempSharedKeys();
        return sk;
    }


    SharedKeys DBAccess::updateTempSharedKeys() {
        auto &db = _insertionDB ? *_insertionDB : *this;
        SharedKeys result;
        return db.useLocked<SharedKeys>([&](C4Database *idb) {
            SharedKeys dbsk = idb->getFleeceSharedKeys();
            lock_guard<mutex> lock(_tempSharedKeysMutex);
            if (!_tempSharedKeys || _tempSharedKeysInitialCount < dbsk.count()) {
                // Copy database's sharedKeys:
                _tempSharedKeys = SharedKeys::create(dbsk.stateData());
                _tempSharedKeysInitialCount = dbsk.count();
                assert(_tempSharedKeys);
            }
            return _tempSharedKeys;
        });
    }


    Doc DBAccess::tempEncodeJSON(slice jsonBody, FLError *err) {
        Encoder enc;
        enc.setSharedKeys(tempSharedKeys());
        if(!enc.convertJSON(jsonBody)) {
            *err = enc.error();
            WarnError("Fleece encoder convertJSON failed (%d)", *err);
            return {};
        }

        Doc doc = enc.finishDoc();
        if (!doc && err) {
            WarnError("Fleece encoder finishDoc failed (%d)", *err);
            *err = enc.error();
        }

        return doc;
    }


    alloc_slice DBAccess::reEncodeForDatabase(Doc doc) {
        bool reEncode;
        {
            lock_guard<mutex> lock(_tempSharedKeysMutex);
            reEncode = doc.sharedKeys() != _tempSharedKeys
                        || _tempSharedKeys.count() > _tempSharedKeysInitialCount;
        }
        if (reEncode) {
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


    Doc DBAccess::applyDelta(C4Document *doc,
                             slice deltaJSON,
                             bool useDBSharedKeys)
    {
        Dict srcRoot = doc->getProperties();
        if (!srcRoot)
            error::_throw(error::CorruptRevisionData, "DBAccess applyDelta error getting document's properties");

        bool useLegacyAttachments = !_disableBlobSupport && containsAttachmentsProperty(deltaJSON);
        Doc reEncodedDoc;
        if (useLegacyAttachments || !useDBSharedKeys) {
            Encoder enc;
            enc.setSharedKeys(tempSharedKeys());
            if (useLegacyAttachments) {
                // Delta refers to legacy attachments, so convert my base revision to have them:
                encodeRevWithLegacyAttachments(enc, srcRoot, 1);
            } else {
                // Can't use DB SharedKeys, so re-encode to temp encoder
                enc.writeValue(srcRoot);
            }
            reEncodedDoc = enc.finishDoc();
            srcRoot = reEncodedDoc.root().asDict();
        }
        
        Doc result;
        FLError flErr;
        if (useDBSharedKeys) {
            // insertionDB() asserts DB open, no need to do it here
            insertionDB().useLocked([&](C4Database *idb) {
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
        ++gNumDeltasApplied;

        if (!result) {
            if (flErr == kFLInvalidData)
                error::_throw(error::CorruptDelta, "Invalid delta");
            else
                error::_throw(error::Fleece, flErr);
        }
        return result;
    }


    Doc DBAccess::applyDelta(slice docID,
                             slice baseRevID,
                             slice deltaJSON)
    {
        Retained<C4Document> doc = getDoc(docID, kDocGetAll);
        if (!doc)
            error::_throw(error::NotFound);
        if (!doc->selectRevision(baseRevID, true) || !doc->loadRevisionBody())
            return nullptr;
        return applyDelta(doc, deltaJSON, false);
    }


    void DBAccess::markRevSynced(ReplicatedRev *rev NONNULL) {
        _revsToMarkSynced.push(rev);
    }


    // Mark all the queued revisions as synced to the server.
    void DBAccess::markRevsSyncedNow() {
        _timer.stop();
        auto revs = _revsToMarkSynced.pop();
        if (!revs)
            return;

        Stopwatch st;
        // insertionDB() asserts DB open, no need to do it here
        insertionDB().useLocked([&](C4Database *idb) {
            try {
                C4Database::Transaction transaction(idb);
                for (ReplicatedRev *rev : *revs) {
                    logDebug("Marking rev '%.*s' %.*s (#%" PRIu64 ") as synced to remote db %u",
                             SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence, remoteDBID());
                    try {
                        idb->getDefaultCollection()
                          ->markDocumentSynced(rev->docID, rev->revID, rev->sequence, remoteDBID());
                    } catch (const exception &x) {
                        C4Error error = C4Error::fromException(x);
                        warn("Unable to mark '%.*s' %.*s (#%" PRIu64 ") as synced; error %d/%d",
                             SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence,
                             error.domain, error.code);
                    }
                }
                transaction.commit();
                double t = st.elapsed();
                logVerbose("Marked %zu revs as synced-to-server in %.2fms (%.0f/sec)",
                           revs->size(), t*1000, revs->size()/t);
            } catch (const exception &x) {
                C4Error error = C4Error::fromException(x);
                warn("Error marking %zu revs as synced: %d/%d", revs->size(), error.domain, error.code);
            }
        });
    }


    void DBAccess::markRevsSyncedLater() {
        _timer.fireAfter(tuning::kInsertionDelay);
    }


    atomic<unsigned> DBAccess::gNumDeltasApplied;

    
} }
