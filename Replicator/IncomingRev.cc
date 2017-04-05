//
//  IncomingRev.cc
//  LiteCore
//
//  Created by Jens Alfke on 3/30/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "IncomingRev.hh"
#include "IncomingBlob.hh"
#include "DBWorker.hh"
#include "Puller.hh"
#include "StringUtil.hh"
#include "BLIP.hh"

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore::blip;

namespace litecore { namespace repl {

    static bool hasUnderscoredProperties(Dict);
    static alloc_slice stripUnderscoredProperties(Dict);
    static void findBlobReferences(Dict, vector<BlobRequest> &keys);


    IncomingRev::IncomingRev(Puller *puller, DBWorker *dbActor)
    :Worker(puller, "inc")
    ,_puller(puller)
    ,_dbActor(dbActor)
    {
        _important = false;
    }


    // Resets the object so it can be reused for another revision.
    void IncomingRev::clear() {
        assert(_pendingCallbacks == 0 && _pendingBlobs == 0);
        _revMessage = nullptr;
        _rev.clear();
        _error = {};
    }

    
    // Read the 'rev' message, on my actor thread:
    void IncomingRev::_handleRev(Retained<blip::MessageIn> msg) {
        assert(!_revMessage);
        _revMessage = msg;

        // Convert JSON to Fleece:
        FLError err;
        alloc_slice fleeceBody = Encoder::convertJSON(_revMessage->body(), &err);
        if (!fleeceBody) {
            gotError(C4Error{FleeceDomain, err});
            return;
        }
        Dict root = Value::fromTrustedData(fleeceBody).asDict();

        // Populate the RevToInsert's metadata:
        bool stripUnderscores;
        _rev.docID = _revMessage->property("id"_sl);
        if (_rev.docID) {
            _rev.revID = _revMessage->property("rev"_sl);
            if (_revMessage->property("deleted"_sl))
                _rev.flags |= kRevDeleted;
            stripUnderscores = hasUnderscoredProperties(root);
        } else {
            // No metadata properties; look inside the JSON:
            _rev.docID = (slice)root["_id"_sl].asString();
            _rev.revID = (slice)root["_rev"_sl].asString();
            if (root["_deleted"].asBool())
                _rev.flags = kRevDeleted;
            stripUnderscores = true;
        }
        _rev.historyBuf = _revMessage->property("history"_sl);
        slice sequence(_revMessage->property("sequence"_sl));

        // Validate:
        logVerbose("Received revision '%.*s' #%.*s (seq '%.*s')",
                   SPLAT(_rev.docID), SPLAT(_rev.revID), SPLAT(sequence));
        if (_rev.docID.size == 0 || _rev.revID.size == 0) {
            warn("Got invalid revision");
            _revMessage->respondWithError({"BLIP"_sl, 400, "invalid revision"_sl});
            return;
        }
        if (nonPassive() && !sequence) {
            warn("Missing sequence in 'rev' message for active puller");
            _revMessage->respondWithError({"BLIP"_sl, 400, "missing sequence"_sl});
            return;
        }

        // Populate the RevToInsert's body:
        if (stripUnderscores) {
            fleeceBody = stripUnderscoredProperties(root);
            root = Value::fromTrustedData(fleeceBody).asDict();
        }
        _rev.body = fleeceBody;

        // Check for blobs:
        vector<BlobRequest> blobs;
        findBlobReferences(root, blobs);
        if (blobs.empty()) {
            // No blobs, so insert immediately:
            insertRevision();
        } else {
            _rev.flags |= kRevHasAttachments;
            _dbActor->findBlobs(move(blobs), asynchronize([=](vector<BlobRequest> missing,
                                                              C4BlobStore *blobStore) {
                if (missing.empty()) {
                    insertRevision();
                } else {
                    for (auto &request : missing) {
                        Retained<IncomingBlob> b(new IncomingBlob(this, blobStore));
                        b->start(request);
                        ++_pendingBlobs;
                    }
                }
            }));
        }
    }


    void IncomingRev::_childChangedStatus(Worker *task, Status status) {
        addProgress(status.progressDelta);
        if (status.level == kC4Stopped) {
            if (status.error.code && !_error.code)
                _error = status.error;
            assert(_pendingBlobs > 0);
            if (--_pendingBlobs == 0) {
                // All blobs completed, now finish:
                if (_error.code == 0) {
                    logVerbose("All blobs received, now inserting revision");
                    insertRevision();
                } else {
                    finish();
                }
            }
        }
    }


    // Asks the DBAgent to insert the revision, then sends the reply and notifies the Puller.
    void IncomingRev::insertRevision() {
        ++_pendingCallbacks;
        _rev.onInserted = asynchronize([this](C4Error err) {
            // Callback that will run _after_ insertRevision() completes:
            --_pendingCallbacks;
            finish();
        });

        _dbActor->insertRevision(&_rev);
    }


    void IncomingRev::finish() {
        if (!_revMessage->noReply()) {
            MessageBuilder response(_revMessage);
            if (_error.code != 0)
                response.makeError(c4ToBLIPError(_error));
            _revMessage->respond(response);
        }
        _puller->revWasHandled(this, remoteSequence(), (_error.code == 0));
        clear();
    }


    Worker::ActivityLevel IncomingRev::computeActivityLevel() const {
        if (Worker::computeActivityLevel() == kC4Busy
                || _pendingCallbacks > 0 || _pendingBlobs > 0) {
            return kC4Busy;
        } else {
            return kC4Stopped;
        }
    }


#pragma mark - UTILITIES:


    // Returns true if a Fleece Dict contains any keys that begin with an underscore.
    static bool hasUnderscoredProperties(Dict root) {
        for (Dict::iterator i(root); i; ++i) {
            auto key = slice(i.keyString());
            if (key.size > 0 && key[0] == '_')
                return true;
        }
        return false;
    }


    // Encodes a Dict, skipping top-level properties whose names begin with an underscore.
    static alloc_slice stripUnderscoredProperties(Dict root) {
        Encoder e;
        e.beginDict(root.count());
        for (Dict::iterator i(root); i; ++i) {
            auto key = slice(i.keyString());
            if (key.size > 0 && key[0] == '_')
                continue;
            e.writeKey(key);
            e.writeValue(i.value());
        }
        e.endDict();
        return e.finish();
    }


    // Finds blob references in a Fleece value, recursively.
    static void findBlobReferences(Value val, vector<BlobRequest> &blobs) {
        auto d = val.asDict();
        if (d) {
            findBlobReferences(d, blobs);
            return;
        }
        auto a = val.asArray();
        if (a) {
            for (Array::iterator i(a); i; ++i)
                findBlobReferences(i.value(), blobs);
        }
    }


    // Finds blob references in a Fleece Dict, recursively.
    static void findBlobReferences(Dict dict, vector<BlobRequest> &blobs) {
        if (slice(dict["_cbltype"_sl].asString()) == "blob"_sl) {
            C4BlobKey key;
            auto digest = dict["digest"_sl].asString();
            uint64_t length = dict["length"_sl].asUnsigned();
            if (c4blob_keyFromString(digest, &key))
                blobs.push_back({key, length});
        } else {
            for (Dict::iterator i(dict); i; ++i)
                findBlobReferences(i.value(), blobs);
        }
    }

} }

