//
// DBAccess.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "DBAccess.hh"
#include "ReplicatedRev.hh"
#include "ReplicatorTuning.hh"
#include "Error.hh"
#include "Stopwatch.hh"
#include "StringUtil.hh"
#include "c4Private.h"
#include "c4BlobStore.h"
#include "c4Document+Fleece.h"
#include "c4DocEnumerator.h"
#include "c4Private.h"
#include "c4Transaction.hh"
#include <functional>
#include <set>
#include <utility>


namespace litecore { namespace repl {

    using namespace std;
    using namespace fleece;


    DBAccess::DBAccess(C4Database* db, bool disableBlobSupport)
    :access_lock(move(db))
    ,Logging(SyncLog)
    ,_blobStore(c4db_getBlobStore(db, nullptr))
    ,_disableBlobSupport(disableBlobSupport)
    ,_revsToMarkSynced(bind(&DBAccess::markRevsSyncedNow, this),
                       bind(&DBAccess::markRevsSyncedLater, this),
                       tuning::kInsertionDelay)
    ,_timer(bind(&DBAccess::markRevsSyncedNow, this))
    {
        c4db_retain(db);
        // Copy database's sharedKeys:
        SharedKeys dbsk = c4db_getFLSharedKeys(db);
    }


    access_lock<C4Database*>& DBAccess::insertionDB() {
        if (!_insertionDB) {
            use([&](C4Database *db) {
                if (!_insertionDB) {
                    C4Error error;
                    C4Database *idb = c4db_openAgain(db, &error);
                    if (!idb) {
                        logError("Couldn't open new db connection: %s", c4error_descriptionStr(error));
                        idb = c4db_retain(db);
                    } else {
                        c4db_setDatabaseTag(idb, DatabaseTag_DBAccess);
                    }
                    _insertionDB.reset(new access_lock<C4Database*>(move(idb)));
                }
            });
        }
        return *_insertionDB;
    }


    DBAccess::~DBAccess() {
        close();
    }


    void DBAccess::close() {
        _timer.stop();
        use([&](C4Database *&db) {
            // Any use of the class after this will result in a crash that 
            // should be easily identifiable, so forgo asserting if the pointer 
            // is null in other areas.
            c4db_release(db);
            db = nullptr;
        });
        if (_insertionDB) {
            _insertionDB->use([&](C4Database *idb) {
                c4db_release(idb);
            });

            _insertionDB.reset();
        }
    }


    C4RemoteID DBAccess::lookUpRemoteDBID(slice key, C4Error *outError) {
        Assert(_remoteDBID == 0);
        use([&](C4Database *db) {
            _remoteDBID = c4db_getRemoteDBID(db, key, true, outError);
        });
        return _remoteDBID;
    }


    Dict DBAccess::getDocRoot(C4Document *doc, C4RevisionFlags *outFlags) {
        slice revisionBody(doc->selectedRev.body);
        if (!revisionBody)
            return nullptr;
        if (outFlags)
            *outFlags = doc->selectedRev.flags;
        return Value::fromData(revisionBody, kFLTrusted).asDict();
    }


    Dict DBAccess::getDocRoot(C4Document *doc, slice revID, C4RevisionFlags *outFlags) {
        if (c4doc_selectRevision(doc, revID, true, nullptr) && c4doc_loadRevisionBody(doc, nullptr))
            return getDocRoot(doc, outFlags);
        return nullptr;
    }


    alloc_slice DBAccess::getDocRemoteAncestor(C4Document *doc) {
        if (_remoteDBID)
            return c4doc_getRemoteAncestor(doc, _remoteDBID);
        else
            return {};
    }
    
    void DBAccess::setDocRemoteAncestor(slice docID, slice revID) {
        if (!_remoteDBID)
            return;
        logInfo("Updating remote #%u's rev of '%.*s' to %.*s",
                _remoteDBID, SPLAT(docID), SPLAT(revID));
        C4Error error;
        bool ok = use<bool>([&](C4Database *db) {
            c4::Transaction t(db);
            c4::ref<C4Document> doc = c4doc_get(db, docID, true, &error);
            if(!doc || !c4doc_selectRevision(doc, revID, false, &error))
                return false;
            
            
            return t.begin(&error)
                   && c4doc_setRemoteAncestor(doc, _remoteDBID, &error)
                   && c4doc_save(doc, 0, &error)
                   && t.commit(&error);
        });

        if (!ok) {
            warn("Failed to update remote #%u's rev of '%.*s' to %.*s: %d/%d",
                 _remoteDBID, SPLAT(docID), SPLAT(revID), error.domain, error.code);
        }
    }
    
    C4DocEnumerator* DBAccess::unresolvedDocsEnumerator(bool orderByID, C4Error *outError) {
        C4DocEnumerator* e;
        use([&](C4Database *db) {
            C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
            options.flags &= ~kC4IncludeBodies;
            options.flags &= ~kC4IncludeNonConflicted;
            options.flags |= kC4IncludeDeleted;
            if (!orderByID)
                options.flags |= kC4Unsorted;
            e = c4db_enumerateAllDocs(db, &options, outError);
        });
        return e;
    }


    static bool containsAttachmentsProperty(slice json) {
        if (!json.find("\"_attachments\":"_sl))
            return false;
        Doc doc = Doc::fromJSON(json);
        return doc.root().asDict()["_attachments"] != nullptr;
    }


    static inline bool isAttachment(FLDeepIterator i, C4BlobKey *blobKey, bool noBlobs) {
        auto dict = FLValue_AsDict(FLDeepIterator_GetValue(i));
        if (!dict)
            return false;
        if (!noBlobs && c4doc_dictIsBlob(dict, blobKey))
            return true;
        FLPathComponent* path;
        size_t depth;
        FLDeepIterator_GetPath(i, &path, &depth);
        return depth == 2
        && FLSlice_Equal(path[0].key, FLSTR(kC4LegacyAttachmentsProperty))
        && c4doc_getDictBlobKey(dict, blobKey);
    }


    void DBAccess::findBlobReferences(Dict root, bool unique, const FindBlobCallback &callback) {
        // This method is non-static because it references _disableBlobSupport, but it's
        // thread-safe.
        set<string> found;
        FLDeepIterator i = FLDeepIterator_New(root);
        for (; FLDeepIterator_GetValue(i); FLDeepIterator_Next(i)) {
            C4BlobKey blobKey;
            if (isAttachment(i, &blobKey, _disableBlobSupport)) {
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
            if (key == slice(kC4LegacyAttachmentsProperty)) {
                oldAttachments = i.value().asDict();    // remember _attachments dict for later
            } else {
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Now write _attachments:
        enc.writeKey(slice(kC4LegacyAttachmentsProperty));
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
                if (key != slice(kC4ObjectTypeProperty) && key != "stub"_sl) {
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
        auto db = _insertionDB.get();
        if (!db) db = this;
        SharedKeys result;
        return db->use<SharedKeys>([&](C4Database *idb) {
            SharedKeys dbsk = c4db_getFLSharedKeys(idb);
            lock_guard<mutex> lock(_tempSharedKeysMutex);
            if (!_tempSharedKeys || _tempSharedKeysInitialCount < dbsk.count()) {
                // Copy database's sharedKeys:
                _tempSharedKeys = SharedKeys::create(dbsk.stateData());
                _tempSharedKeysInitialCount = dbsk.count();
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
            return useForInsert<alloc_slice>([&](C4Database* idb) {
                SharedEncoder enc(c4db_getSharedFleeceEncoder(idb));
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


    Doc DBAccess::applyDelta(const C4Revision *baseRevision,
                             slice deltaJSON,
                             bool useDBSharedKeys,
                             C4Error *outError)
    {
        Dict srcRoot = Value::fromData(baseRevision->body, kFLTrusted).asDict();
        if (!srcRoot) {
            if (outError) *outError = c4error_make(LiteCoreDomain, kC4ErrorCorruptRevisionData, nullslice);
            return {};
        }

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
            useForInsert([&](C4Database *idb) {
                SharedEncoder enc(c4db_getSharedFleeceEncoder(idb));
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

        if (!result && outError) {
            if (flErr == kFLInvalidData)
                *outError = c4error_make(LiteCoreDomain, kC4ErrorCorruptDelta, "Invalid delta"_sl);
            else
                *outError = {FleeceDomain, flErr};
        }
        return result;
    }


    Doc DBAccess::applyDelta(slice docID,
                             slice baseRevID,
                             slice deltaJSON,
                             C4Error *outError)
    {
        return useForInsert<Doc>([&](C4Database *idb)->Doc {
            c4::ref<C4Document> doc = c4doc_get(idb, docID, true, outError);
            if (doc && c4doc_selectRevision(doc, baseRevID, true, outError)) {
                if (doc->selectedRev.body.buf) {
                    return applyDelta(&doc->selectedRev, deltaJSON, false, outError);
                } else {
                    string msg = format("Couldn't apply delta: Don't have body of '%.*s' #%.*s [current is %.*s]",
                                        SPLAT(docID), SPLAT(baseRevID), SPLAT(doc->revID));
                    *outError = c4error_make(LiteCoreDomain, kC4ErrorDeltaBaseUnknown, slice(msg));
                }
            }
            return nullptr;
        });
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
        useForInsert([&](C4Database *idb) {
            C4Error error;
            c4::Transaction transaction(idb);
            if (transaction.begin(&error)) {
                for (ReplicatedRev *rev : *revs) {
                    logDebug("Marking rev '%.*s' %.*s (#%llu) as synced to remote db %u",
                             SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence, remoteDBID());
                    if (!c4db_markSynced(idb, rev->docID, rev->sequence, remoteDBID(), &error))
                        warn("Unable to mark '%.*s' %.*s (#%" PRIu64 ") as synced; error %d/%d",
                             SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence, error.domain, error.code);
                }
                if (transaction.commit(&error)) {
                    double t = st.elapsed();
                    logVerbose("Marked %zu revs as synced-to-server in %.2fms (%.0f/sec)",
                            revs->size(), t*1000, revs->size()/t);
                    return;
                }
            }
            warn("Error marking %zu revs as synced: %d/%d", revs->size(), error.domain, error.code);
        });
    }


    void DBAccess::markRevsSyncedLater() {
        _timer.fireAfter(tuning::kInsertionDelay);
    }


    bool DBAccess::beginTransaction(C4Error *outError) {
        return useForInsert<bool>([&](C4Database *idb) {
            Assert(!_inTransaction);
            _inTransaction = c4db_beginTransaction(idb, outError);
            return _inTransaction;
        });
    }

    bool DBAccess::endTransaction(bool commit, C4Error *outError) {
        return useForInsert<bool>([&](C4Database *idb) {
            Assert(_inTransaction);
            _inTransaction = false;
            return c4db_endTransaction(idb, commit, outError);
        });
    }



    atomic<unsigned> DBAccess::gNumDeltasApplied;

    
} }
