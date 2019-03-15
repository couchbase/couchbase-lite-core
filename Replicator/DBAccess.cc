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
#include <set>


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


    DBAccess::~DBAccess() {
        use([&](C4Database *db) {
            c4db_free(db);
        });
    }


    std::string DBAccess::loggingClassName() const {
        return "DBAccess";
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
        if (!_tempSharedKeys)
            updateTempSharedKeys();
        SharedKeys sk;
        {
            lock_guard<mutex> lock(_tempSharedKeysMutex);
            sk = _tempSharedKeys;
        }
        return sk;
    }


    bool DBAccess::updateTempSharedKeys() {
        use([&](C4Database *db) {
            SharedKeys dbsk = c4db_getFLSharedKeys(db);
            lock_guard<mutex> lock(_tempSharedKeysMutex);
            if (!_tempSharedKeys || _tempSharedKeysInitialCount < dbsk.count()) {
                // Copy database's sharedKeys:
                _tempSharedKeys = SharedKeys::create(dbsk.stateData());
                _tempSharedKeysInitialCount = dbsk.count();
            }
        });
        return true;
    }


    Doc DBAccess::tempEncodeJSON(slice jsonBody, FLError *err) {
        Encoder enc;
        enc.setSharedKeys(tempSharedKeys());
        enc.convertJSON(jsonBody);
        Doc doc = enc.finishDoc();
        if (!doc && err)
            *err = enc.error();
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
            return use<alloc_slice>([&](C4Database* db) {
                SharedEncoder enc(c4db_getSharedFleeceEncoder(db));
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

        Doc legacy;
        if (!_disableBlobSupport && containsAttachmentsProperty(deltaJSON)) {
            // Delta refers to legacy attachments, so convert my base revision to have them:
            Encoder enc;
            encodeRevWithLegacyAttachments(enc, srcRoot, 1);
            legacy = enc.finishDoc();
            srcRoot = legacy.root().asDict();
        }

        Doc result;
        FLError flErr;
        if (useDBSharedKeys) {
            use([&](C4Database *db) {
                FLEncoder enc = c4db_getSharedFleeceEncoder(db);
                FLEncodeApplyingJSONDelta(srcRoot, deltaJSON, enc);
                result = FLEncoder_FinishDoc(enc, &flErr);
            });
        } else {
            Encoder enc;
            enc.setSharedKeys(tempSharedKeys());
            FLEncodeApplyingJSONDelta(srcRoot, deltaJSON, enc);
            result = FLEncoder_FinishDoc(enc, &flErr);
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
        return use<Doc>([&](C4Database *db)->Doc {
            c4::ref<C4Document> doc = c4doc_get(db, docID, true, outError);
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


    // Mark all the queued revisions as synced to the server.
    void DBAccess::markRevsSyncedNow() {
        _timer.stop();
        auto revs = _revsToMarkSynced.pop();
        if (!revs)
            return;

        use([&](C4Database *db) {
            Stopwatch st;
            C4Error error;
            c4::Transaction transaction(db);
            if (transaction.begin(&error)) {
                for (ReplicatedRev *rev : *revs) {
                    logDebug("Marking rev '%.*s' %.*s (#%llu) as synced to remote db %u",
                             SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence, remoteDBID());
                    if (!c4db_markSynced(db, rev->docID, rev->sequence, remoteDBID(), &error))
                        warn("Unable to mark '%.*s' %.*s (#%llu) as synced; error %d/%d",
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


    atomic<unsigned> DBAccess::gNumDeltasApplied;

    
} }
