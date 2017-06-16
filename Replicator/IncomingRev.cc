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
#include "c4Document+Fleece.h"
#include "BLIP.hh"

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore::blip;

namespace litecore { namespace repl {

    using FindBlobCallback = function<void(const C4BlobKey &key, uint64_t size)>;
    static void findBlobReferences(Dict, const FindBlobCallback&);


    IncomingRev::IncomingRev(Puller *puller, DBWorker *dbWorker)
    :Worker(puller, "inc")
    ,_puller(puller)
    ,_dbWorker(dbWorker)
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

        // Parse the JSON to Fleece. This Fleece data is _not_ suitable for inserting into the
        // database because it doesn't use the SharedKeys, but it lets us look at the doc
        // metadata and blobs.
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
            stripUnderscores = c4doc_hasOldMetaProperties(root);
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
            fleeceBody = c4doc_encodeStrippingOldMetaProperties(root);
            root = Value::fromTrustedData(fleeceBody).asDict();
        }
        _rev.body = fleeceBody;

        // Check for blobs:
        auto blobStore = _dbWorker->blobStore();
        findBlobReferences(root, [=](const C4BlobKey &key, uint64_t size) {
            if (c4blob_getSize(blobStore, key) < 0) {
                Retained<IncomingBlob> b(new IncomingBlob(this, blobStore));
                b->start(key, size);
                ++_pendingBlobs;
            }
        });
        if (_pendingBlobs > 0)
            _rev.flags |= kRevHasAttachments;
        else
            insertRevision();
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

        _dbWorker->insertRevision(&_rev);
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


    // Finds blob references in a Fleece value, recursively.
    static void findBlobReferences(Value val, const FindBlobCallback &callback) {
        auto d = val.asDict();
        if (d) {
            findBlobReferences(d, callback);
            return;
        }
        auto a = val.asArray();
        if (a) {
            for (Array::iterator i(a); i; ++i)
                findBlobReferences(i.value(), callback);
        }
    }


    // Finds blob references in a Fleece Dict, recursively.
    static void findBlobReferences(Dict dict, const FindBlobCallback &callback)
    {
        if (dict["_cbltype"_sl]) {
            C4BlobKey key;
            if (c4doc_dictIsBlob(dict, &key)) {
                uint64_t length = dict["length"_sl].asUnsigned();
                callback(key, length);
            }
        } else {
            for (Dict::iterator i(dict); i; ++i)
                findBlobReferences(i.value(), callback);
        }
    }

} }

