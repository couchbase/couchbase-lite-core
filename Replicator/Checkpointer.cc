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

#include "c4Base.hh"
#include "Checkpointer.hh"
#include "Checkpoint.hh"
#include "ReplicatorOptions.hh"
#include "DBAccess.hh"
#include "Base64.hh"
#include "Logging.hh"
#include "SecureDigest.hh"
#include "StringUtil.hh"
#include "c4Database.hh"
#include "DatabaseImpl.hh"
#include <inttypes.h>

#include "c4Database.hh"
#include "c4Document.hh"
#include "c4DocEnumerator.hh"

#define LOCK()  lock_guard<mutex> lock(_mutex)

namespace litecore { namespace repl {
    using namespace std;
    using namespace fleece;


#pragma mark - CHECKPOINT ACCESSORS:


    Checkpointer::Checkpointer(const Options &opt, fleece::slice remoteURL)
    :_options(opt)
    ,_remoteURL(remoteURL)
    { }


    Checkpointer::~Checkpointer() =default;


    string Checkpointer::to_string() const {
        LOCK();
        return _checkpoint->completedSequences().to_string();
    }


    C4SequenceNumber Checkpointer::localMinSequence() const {
        LOCK();
        return _checkpoint->localMinSequence();
    }

    RemoteSequence Checkpointer::remoteMinSequence() const {
        LOCK();
        return _checkpoint->remoteMinSequence();
    }


    void Checkpointer::setRemoteMinSequence(const RemoteSequence &s) {
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

    void Checkpointer::addPendingSequences(const std::vector<C4SequenceNumber> &sequences,
                                           C4SequenceNumber firstInRange,
                                           C4SequenceNumber lastInRange)
    {
        LOCK();
        _checkpoint->addPendingSequences(sequences, firstInRange, lastInRange);
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


    slice Checkpointer::remoteDocID(C4Database *db) {
        if(!_docID)
            _docID = docIDForUUID(db->privateUUID(), URLTransformStrategy::AsIs);
        return _docID;
    }


    slice Checkpointer::remoteDBIDString() const {
        return _options.remoteDBIDString(_remoteURL);
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
    string Checkpointer::docIDForUUID(const C4UUID &localUUID, URLTransformStrategy urlStrategy) {
        // Derive docID from from db UUID, remote URL, channels, filter, and docIDs.
        Array channels = _options.channels();
        Value filter = _options.properties[kC4ReplicatorOptionFilter];
        const Value filterParams = _options.properties[kC4ReplicatorOptionFilterParams];
        Array docIDs = _options.docIDs();

        // Compute the ID by writing the values to a Fleece array, then taking a SHA1 digest:
        fleece::Encoder enc;
        enc.beginArray();
        enc.writeString({&localUUID, sizeof(C4UUID)});

        alloc_slice rawURL(remoteDBIDString());
        auto encodedURL = transform_url(rawURL, urlStrategy);
        if(!encodedURL) {
            return "";
        }

        enc.writeString(encodedURL);
        if (!channels.empty() || !docIDs.empty() || filter) {
            // Optional stuff:
            writeValueOrNull(enc, channels);
            writeValueOrNull(enc, filter);
            writeValueOrNull(enc, filterParams);
            writeValueOrNull(enc, docIDs);
        }
        enc.endArray();
        return string("cp-") + SHA1(enc.finish()).asBase64();
    }


#pragma mark - READING THE CHECKPOINT:


    // Reads the local checkpoint
    bool Checkpointer::read(C4Database *db, bool reset) {
        if (_checkpoint)
            return true;

        alloc_slice body;
        if (_initialDocID) {
            body = _read(db, _initialDocID);
        } else {
            // By default, the local doc ID is the same as the remote one:
            _initialDocID = alloc_slice(remoteDocID(db));
            body = _read(db, _initialDocID);
            if (!body) {
                // Look for a prior database UUID:
                db->getRawDocument(C4Database::kInfoStore, constants::kPreviousPrivateUUIDKey,
                                   [&](C4RawDocument *doc) {
                    if (doc) {
                        // If there is one, derive a doc ID from that and look for a checkpoint there
                        for(URLTransformStrategy strategy = URLTransformStrategy::AddPort; strategy <= URLTransformStrategy::RemovePort; ++strategy) {
                            // CBL-1515: Make sure to account for platform inconsistencies in the format
                            // (some have been forcing the port for standard ports and others were omitting it)
                            _initialDocID = docIDForUUID(*(C4UUID*)doc->body.buf, strategy);
                            if(!_initialDocID) {
                                continue;
                            }

                            body = _read(db, _initialDocID);
                            if (body)
                                break;
                        }
                    }
                });
            }
        }

        // Checkpoint doc is either read, or nonexistent:
        LOCK();
        _checkpoint.reset(new Checkpoint);
        if (body && !reset) {
            _checkpoint->readJSON(body);
            _checkpointJSON = body;
            return true;
        } else {
            return false;
        }
    }


    // subroutine that actually reads the checkpoint doc from the db
    alloc_slice Checkpointer::_read(C4Database *db, slice checkpointID) {
        alloc_slice body;
        db->getRawDocument(constants::kLocalCheckpointStore, checkpointID,
                           [&](C4RawDocument *doc) {
            if (doc)
                body = alloc_slice(doc->body);
        });
        return body;
    }


    void Checkpointer::write(C4Database *db, slice data) {
        const auto checkpointID = remoteDocID(db);
        db->putRawDocument(constants::kLocalCheckpointStore, {checkpointID, nullslice, data});
        // Now that we've saved, use the real checkpoint ID for any future reads:
        _initialDocID = checkpointID;
        _checkpointJSON = nullslice;
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
        return isDocumentIDAllowed(doc->docID())
            && (!_options.pushFilter || _options.pushFilter(doc->docID(),
                                                            doc->selectedRev().revID,
                                                            doc->selectedRev().flags,
                                                            doc->getProperties(),
                                                            _options.callbackContext));
    }


    bool Checkpointer::isDocumentIDAllowed(slice docID) {
        initializeDocIDs();
        return _docIDs.empty() || _docIDs.find(string(docID)) != _docIDs.end();
    }


#pragma mark - PENDING DOCUMENTS:


    void Checkpointer::pendingDocumentIDs(C4Database* db, PendingDocCallback callback) {
        if(_options.push < kC4OneShot) {
            // Couchbase Lite should not allow this case
            C4Error::raise(LiteCoreDomain, kC4ErrorUnsupported);
        }

        read(db, false);
        const auto dbLastSequence = db->getDefaultCollection()->getLastSequence();
        const auto replLastSequence = this->localMinSequence();
        if(replLastSequence >= dbLastSequence) {
            // No changes since the last checkpoint
            return;
        }

        C4EnumeratorOptions opts { kC4IncludeNonConflicted | kC4IncludeDeleted };
        const auto hasDocIDs = bool(_options.docIDs());
        if(!hasDocIDs && _options.pushFilter) {
            // docIDs has precedence over push filter
            opts.flags |= kC4IncludeBodies;
        }

        C4DocEnumerator e(db->getDefaultCollection(), replLastSequence, opts);
        while(e.next()) {
            C4DocumentInfo info = e.documentInfo();

            if (_checkpoint->isSequenceCompleted(info.sequence))
                continue;

            if(!isDocumentIDAllowed(info.docID))
                continue;

            if (!hasDocIDs && _options.pushFilter) {
                // If there is a push filter, we have to get the doc body for it to peruse:
                Retained<C4Document> nextDoc = e.getDocument();
                if(!nextDoc) {
                    Warn("Got non-existent document during pending document IDs, skipping...");
                    continue;
                }

                if(!nextDoc->loadRevisionBody()) {
                    Warn("Error loading revision body in pending document IDs");
                    continue;
                }

                if(!isDocumentAllowed(nextDoc))
                    continue;
            }

            callback(info);
        }
    }


    bool Checkpointer::isDocumentPending(C4Database* db, slice docId) {
        if(_options.push < kC4OneShot) {
            // Couchbase Lite should not allow this case
            C4Error::raise(LiteCoreDomain, kC4ErrorUnsupported);
        }

        read(db, false);
        Retained<C4Document> doc = db->getDefaultCollection()->getDocument(docId, false, kDocGetCurrentRev);
        return doc && !_checkpoint->isSequenceCompleted(doc->sequence()) && isDocumentAllowed(doc);
    }


#pragma mark - STORING PEER CHECKPOINTS:


    bool Checkpointer::getPeerCheckpoint(C4Database* db,
                                         slice checkpointID,
                                         alloc_slice &outBody,
                                         alloc_slice &outRevID)
    {
        return db->getRawDocument(constants::kPeerCheckpointStore, checkpointID,
                                  [&](C4RawDocument *doc) {
            if (doc) {
                outBody = alloc_slice(doc->body);
                outRevID = alloc_slice(doc->meta);
            }
        });
    }


    bool Checkpointer::savePeerCheckpoint(C4Database* db,
                                          slice checkpointID,
                                          slice body,
                                          slice revID,
                                          alloc_slice &newRevID)
    {
        C4Database::Transaction t(db);

        // Get the existing raw doc so we can check its revID:
        alloc_slice actualRev;
        unsigned long generation = 0;
        db->getRawDocument(constants::kPeerCheckpointStore, checkpointID,
                                  [&](C4RawDocument *doc) {
            if (doc) {
                generation = C4Document::getRevIDGeneration(doc->meta);
                if (generation > 0)
                    actualRev = doc->meta;
            };
        });

        // Check for conflict:
        if (revID != actualRev)
            return false;

        // Generate new revID:
        char newRevBuf[20];
        newRevID = alloc_slice(newRevBuf, sprintf(newRevBuf, "%lu-cc", ++generation));

        // Save:
        db->putRawDocument(constants::kPeerCheckpointStore, {checkpointID, newRevID, body});
        t.commit();
        return true;
    }


} }
