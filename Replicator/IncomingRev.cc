//
//  IncomingRev.cc
//  LiteCore
//
//  Created by Jens Alfke on 3/30/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "IncomingRev.hh"
#include "DBActor.hh"
#include "Puller.hh"
#include "StringUtil.hh"

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore::blip;

namespace litecore { namespace repl {

    static bool hasUnderscoredProperties(Dict);
    static alloc_slice stripUnderscoredProperties(Dict);

    
    IncomingRev::IncomingRev(Puller *puller, DBActor *dbActor)
    :ReplActor(puller, "inc")
    ,_puller(puller)
    ,_dbActor(dbActor)
    {
        _important = false;
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
            _rev.deleted = !!_revMessage->property("deleted"_sl);
            stripUnderscores = hasUnderscoredProperties(root);
        } else {
            // No metadata properties; look inside the JSON:
            _rev.docID = (slice)root["_id"_sl].asString();
            _rev.revID = (slice)root["_rev"_sl].asString();
            _rev.deleted = root["_deleted"].asBool();
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
        if (stripUnderscores)
            fleeceBody = stripUnderscoredProperties(root);
        _rev.body = fleeceBody;

        // Finally insert the revision:
        insertRevision();
    }


    void IncomingRev::insertRevision() {
        // Callback that will run after the revision save completes:
        ++_pendingCallbacks;
        _rev.onInserted = asynchronize([this](C4Error err) {
            // Reply to the 'rev' message:
            if (!_revMessage->noReply()) {
                MessageBuilder response(_revMessage);
                if (err.code != 0)
                    response.makeError(c4ToBLIPError(err));
                _revMessage->respond(response);
            }
            // Notify the Puller:
            alloc_slice sequence(_revMessage->property("sequence"_sl));
            _puller->revWasHandled(this, sequence, (err.code == 0));
            --_pendingCallbacks;
            clear();
        });

        _dbActor->insertRevision(&_rev);
    }


    void IncomingRev::clear() {
        assert(_pendingCallbacks == 0);
        _revMessage = nullptr;
        _rev.clear();
    }


#pragma mark - UTILITIES:


    ReplActor::ActivityLevel IncomingRev::computeActivityLevel() const {
        if (ReplActor::computeActivityLevel() == kC4Busy
                || _pendingCallbacks > 0) {
            return kC4Busy;
        } else {
            return kC4Stopped;
        }
    }


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

} }

