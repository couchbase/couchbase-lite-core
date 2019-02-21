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


    void DBAccess::writeRevWithLegacyAttachments(fleece::Encoder& enc, Dict root,
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
                    logInfo("Marked %zu revs as synced-to-server in %.2fms (%.0f/sec)",
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

    
} }
