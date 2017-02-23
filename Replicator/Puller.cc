//
//  Puller.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Puller.hh"
#include "DBActor.hh"

using namespace std;
using namespace fleece;
using namespace fleeceapi;

namespace litecore { namespace repl {


    Puller::Puller(Connection *connection, Replicator *replicator, DBActor *dbActor, Options options)
    :ReplActor(connection, options, string("Pull:") + connection->name())
    ,_replicator(replicator)
    ,_dbActor(dbActor)
    {
        registerHandler("rev",    &Puller::handleRev);
    }

    
    void Puller::start(std::string sinceSequence, const Replicator::Options &options) {
        _lastSequence = _lastSequence;
        _options = options;
        LogTo(SyncLog, "Starting pull from remote seq %s", _lastSequence.c_str());
    }


    void Puller::handleRev(Retained<MessageIn> msg) {
        Rev rev;
        rev.docID = msg->property("id"_sl);
        rev.revID = msg->property("rev"_sl);
        rev.deleted = !!msg->property("del"_sl);
        slice history = msg->property("history"_sl);

        LogTo(SyncLog, "Pull: Received revision '%.*s' #%.*s", SPLAT(rev.docID), SPLAT(rev.revID));
        if (rev.docID.size == 0 || rev.revID.size == 0) {
            Warn("Puller got invalid revision");
            msg->respondWithError("BLIP"_sl, 400);
            return;
        }

        FLError err;
        alloc_slice fleeceBody = Encoder::convertJSON(msg->body(), &err);
        if (!fleeceBody) {
            gotError(C4Error{FleeceDomain, err});
            return;
        }
        _dbActor->insertRevision(rev, history, fleeceBody, [msg](C4Error err) {
            // this callback doesn't run on my thread, but it doesn't matter
            if (err.code)
                msg->respondWithError("LiteCore"_sl, err.code);      //TODO: Proper error domain
            else {
                MessageBuilder response(msg);
                msg->respond(response);
            }
        });
    }


} }
