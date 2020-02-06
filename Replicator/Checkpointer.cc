//
// Checkpointer.cc
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

#include "Checkpointer.hh"
#include "Checkpoint.hh"
#include "DBAccess.hh"
#include "Logging.hh"
#include "SecureDigest.hh"
#include "StringUtil.hh"
#include "c4Database.h"
#include "c4DocEnumerator.h"
#include "c4Private.h"
#include "c4.hh"
#include <inttypes.h>

#define LOCK()  lock_guard<mutex> lock(_mutex)

namespace litecore { namespace repl {
    using namespace std;
    using namespace fleece;


#pragma mark - CHECKPOINT ACCESSORS:


    Checkpointer::Checkpointer(const Options &opt, fleece::slice remoteURL)
    :_options(opt)
    ,_remoteURL(remoteURL)
    ,_resetCheckpoint(_options.properties[kC4ReplicatorResetCheckpoint].asBool())
    { }


    Checkpointer::~Checkpointer()
    { }


    string Checkpointer::to_string() const {
        LOCK();
        return _checkpoint->completedSequences().to_string();
    }


    C4SequenceNumber Checkpointer::localMinSequence() const {
        LOCK();
        return _checkpoint->localMinSequence();
    }

    fleece::alloc_slice Checkpointer::remoteMinSequence() const {
        LOCK();
        return _checkpoint->remoteMinSequence();
    }


    void Checkpointer::setRemoteMinSequence(fleece::slice s) {
        LOCK();
        if (_checkpoint->setRemoteMinSequence(s))
            saveSoon();
    }


    bool Checkpointer::validateWith(const Checkpoint &remote) {
        LOCK();
        if (_checkpoint->validateWith(remote))
            return true;
        saveSoon();
        return false;
    }


    void Checkpointer::addPendingSequence(C4SequenceNumber s) {
        LOCK();
        _checkpoint->addPendingSequence(s);
        saveSoon();
    }

    void Checkpointer::addPendingSequences(RevToSendList &sequences,
                                           C4SequenceNumber firstInRange,
                                           C4SequenceNumber lastInRange) {
        LOCK();
        _checkpoint->addPendingSequences(sequences, firstInRange, lastInRange);
        saveSoon();
    }

    void Checkpointer::completedSequence(C4SequenceNumber s) {
        LOCK();
        _checkpoint->completedSequence(s);
        saveSoon();
    }

    bool Checkpointer::isSequenceCompleted(C4SequenceNumber seq) const {
        LOCK();
        return _checkpoint->isSequenceCompleted(seq);
    }

    size_t Checkpointer::pendingSequenceCount() const {
        LOCK();
        return _checkpoint ? _checkpoint->pendingSequenceCount() : 0;
    }


#pragma mark - AUTOSAVE:


    void Checkpointer::enableAutosave(duration saveTime, SaveCallback cb) {
        DebugAssert(saveTime > duration(0));
        LOCK();
        _saveCallback = cb;
        _saveTime = saveTime;
        _timer.reset( new actor::Timer( bind(&Checkpointer::save, this) ) );
    }


    void Checkpointer::stopAutosave() {
        LOCK();
        _timer.reset();
        _changed = false;
    }


    void Checkpointer::saveSoon() {
        // mutex must be locked
        if (_timer) {
            _changed = true;
            if (!_saving && !_timer->scheduled())
                _timer->fireAfter(_saveTime);
        }
    }


    bool Checkpointer::save() {
        alloc_slice json;
        {
            LOCK();
            if (!_changed || !_timer)
                return true;
            if (_saving) {
                // Can't save immediately because a save is still in progress.
                // Remember that I'm in this state, so when save finishes I can re-save.
                _overdueForSave = true;
                return false;
            }
            Assert(_checkpoint);
            _changed = false;
            _saving = true;
            json = _checkpoint->toJSON();
        }
        _saveCallback(json);
        return true;
    }


    void Checkpointer::saveCompleted() {
        bool saveAgain = false;
        {
            LOCK();
            if (_saving) {
                _saving = false;
                if (_overdueForSave)
                    saveAgain = true;
                else if (_changed)
                    _timer->fireAfter(_saveTime);
            }
        }
        if (saveAgain)
            save();
    }

    
    bool Checkpointer::isUnsaved() const {
        LOCK();
        return _changed || _saving;
    }


#pragma mark - CHECKPOINT DOC ID:


    slice Checkpointer::remoteDocID(C4Database *db, C4Error* err) {
        if(!_docID) {
            C4UUID privateID;
            if (!c4db_getUUIDs(db, nullptr, &privateID, err))
                return nullslice;
            _docID = docIDForUUID(privateID);
        }

        return _docID;
    }


    // Writes a Value to an Encoder, substituting null if the value is an empty array.
    static void writeValueOrNull(fleece::Encoder &enc, Value val) {
        auto a = val.asArray();
        if (!val || (a && a.empty()))
            enc.writeNull();
        else
            enc.writeValue(val);
    }


    // Computes the ID of the checkpoint document.
    string Checkpointer::docIDForUUID(const C4UUID &localUUID) {
        // Derive docID from from db UUID, remote URL, channels, filter, and docIDs.
        Array channels = _options.channels();
        Value filter = _options.properties[kC4ReplicatorOptionFilter];
        const Value filterParams = _options.properties[kC4ReplicatorOptionFilterParams];
        Array docIDs = _options.docIDs();

        // Compute the ID by writing the values to a Fleece array, then taking a SHA1 digest:
        fleece::Encoder enc;
        enc.beginArray();
        enc.writeString({&localUUID, sizeof(C4UUID)});

        enc.writeString(_options.remoteDBIDString(_remoteURL));
        if (!channels.empty() || !docIDs.empty() || filter) {
            // Optional stuff:
            writeValueOrNull(enc, channels);
            writeValueOrNull(enc, filter);
            writeValueOrNull(enc, filterParams);
            writeValueOrNull(enc, docIDs);
        }
        enc.endArray();
        const alloc_slice data = enc.finish();
        SHA1 digest(data);
        string finalProduct = string("cp-") + slice(&digest, sizeof(digest)).base64String();
        return finalProduct;
    }


#pragma mark - READING THE CHECKPOINT:


    static inline bool isNotFoundError(C4Error err) {
        return err.domain == LiteCoreDomain && err.code == kC4ErrorNotFound;
    }


    // Reads the local checkpoint
    bool Checkpointer::read(C4Database *db, C4Error *outError) {
        if (_checkpoint)
            return true;

        alloc_slice body;
        if (_initialDocID) {
            body = _read(db, _initialDocID, outError);
        } else {
            // By default, the local doc ID is the same as the remote one:
            _initialDocID = alloc_slice(remoteDocID(db, outError));
            if (!_initialDocID)
                return false;

            body = _read(db, _initialDocID, outError);
            if (!body) {
                if (!isNotFoundError(*outError))
                    return false;

                // Look for a prior database UUID:
                const c4::ref<C4RawDocument> doc = c4raw_get(db, kC4InfoStore,
                                                             constants::kPreviousPrivateUUIDKey, outError);
                if (doc) {
                    // If there is one, derive a doc ID from that and look for a checkpoint there:
                    _initialDocID = docIDForUUID(*(C4UUID*)doc->body.buf);
                    body = _read(db, _initialDocID, outError);
                    if (!body && !isNotFoundError(*outError))
                        return false;
                } else if (!isNotFoundError(*outError)) {
                    return false;
                }
            }
        }

        // Checkpoint doc is either read, or nonexistent:
        LOCK();
        _checkpoint.reset(new Checkpoint);
        if (body && !_resetCheckpoint) {
            _checkpoint->readJSON(body);
            _checkpointJSON = body;
            return true;
        } else {
            *outError = {};
            return false;
        }
    }


    // subroutine that actually reads the checkpoint doc from the db
    alloc_slice Checkpointer::_read(C4Database *db, slice checkpointID, C4Error* err) {
        const c4::ref<C4RawDocument> doc( c4raw_get(db, constants::kLocalCheckpointStore,
                                                    checkpointID, err) );
        return doc ? alloc_slice(doc->body) : nullslice;
    }


    bool Checkpointer::write(C4Database *db, slice data, C4Error *outError) {
        const auto checkpointID = remoteDocID(db, outError);
        if (!checkpointID || !c4raw_put(db, constants::kLocalCheckpointStore,
                                         checkpointID, nullslice, data, outError))
            return false;
        // Now that we've saved, use the real checkpoint ID for any future reads:
        _initialDocID = checkpointID;
        _resetCheckpoint = false;
        _checkpointJSON = nullslice;
        return true;
    }


#pragma mark - DOC-ID FILTER:


    void Checkpointer::initializeDocIDs() {
        if(!_docIDs.empty() || !_options.docIDs() || _options.docIDs().empty()) {
            return;
        }

        Array::iterator i(_options.docIDs());
        while(i) {
            string docID = i.value().asString().asString();
            if(!docID.empty()) {
                _docIDs.insert(docID);
            }

            ++i;
        }
    }


    bool Checkpointer::isDocumentAllowed(C4Document* doc) {
        return isDocumentIDAllowed(doc->docID)
            && (!_options.pushFilter || _options.pushFilter(doc->docID,
                                                            doc->selectedRev.revID,
                                                            doc->selectedRev.flags,
                                                            DBAccess::getDocRoot(doc),
                                                            _options.callbackContext));
    }


    bool Checkpointer::isDocumentIDAllowed(slice docID) {
        initializeDocIDs();
        return _docIDs.empty() || _docIDs.find(string(docID)) != _docIDs.end();
    }


#pragma mark - PENDING DOCUMENTS:


    bool Checkpointer::pendingDocumentIDs(C4Database* db, PendingDocCallback callback,
                                          C4Error* outErr)
    {
        if(_options.push < kC4OneShot) {
            // Couchbase Lite should not allow this case
            outErr->code = kC4ErrorUnsupported;
            outErr->domain = LiteCoreDomain;
            return false;
        }

        if(!read(db, outErr) && outErr->code != 0)
            return false;

        const auto dbLastSequence = c4db_getLastSequence(db);
        const auto replLastSequence = this->localMinSequence();
        if(replLastSequence >= dbLastSequence) {
            // No changes since the last checkpoint
            return true;
        }

        C4EnumeratorOptions opts { kC4IncludeNonConflicted | kC4IncludeDeleted };
        const auto hasDocIDs = bool(_options.docIDs());
        if(!hasDocIDs && _options.pushFilter) {
            // docIDs has precedence over push filter
            opts.flags |= kC4IncludeBodies;
        }

        c4::ref<C4DocEnumerator> e = c4db_enumerateChanges(db, replLastSequence, &opts, outErr);
        if(!e) {
            WarnError("Unable to enumerate changes for pending document IDs (%d / %d)", outErr->domain, outErr->code);
            return false;
        }

        C4DocumentInfo info;
        outErr->code = 0;
        while(c4enum_next(e, outErr)) {
            c4enum_getDocumentInfo(e, &info);

            if (_checkpoint->isSequenceCompleted(info.sequence))
                continue;

            if(!isDocumentIDAllowed(info.docID))
                continue;

            if (!hasDocIDs && _options.pushFilter) {
                // If there is a push filter, we have to get the doc body for it to peruse:
                c4::ref<C4Document> nextDoc = c4enum_getDocument(e, outErr);
                if(!nextDoc) {
                    if(outErr->code != 0)
                        Warn("Error getting document during pending document IDs (%d / %d)",
                             outErr->domain, outErr->code);
                    else
                        Warn("Got non-existent document during pending document IDs, skipping...");
                    continue;
                }

                if(!c4doc_loadRevisionBody(nextDoc, outErr)) {
                    Warn("Error loading revision body in pending document IDs (%d / %d)",
                         outErr->domain, outErr->code);
                    continue;
                }

                if(!isDocumentAllowed(nextDoc))
                    continue;
            }

            callback(info);
        }
        return true;
    }


    bool Checkpointer::isDocumentPending(C4Database* db, slice docId, C4Error* outErr) {
        if(_options.push < kC4OneShot) {
            // Couchbase Lite should not allow this case
            outErr->code = kC4ErrorUnsupported;
            outErr->domain = LiteCoreDomain;
            return false;
        }

        if(!read(db, outErr) && outErr->code != 0)
            return false;

        c4::ref<C4Document> doc = c4doc_get(db, docId, false, outErr);
        if (!doc)
            return false;

        outErr->code = 0;
        return !_checkpoint->isSequenceCompleted(doc->sequence) && isDocumentAllowed(doc);
    }


} }
