//
// c4ConnectedClient_CAPI.cc
//
// Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4ConnectedClient.h"
#include "c4ConnectedClient.hh"
#include "c4Socket+Internal.hh"
#include "c4ExceptionUtils.hh"
#include "Async.hh"
#include "RevID.hh"
#include "DocumentFactory.hh"

using namespace litecore::repl;
using namespace litecore;

C4ConnectedClient* c4client_new(const C4ConnectedClientParameters* params, C4Error *outError) noexcept {
    try {
        return C4ConnectedClient::newClient(*params).detach();
    } catchError(outError);
    return nullptr;
}

void c4client_getDoc(C4ConnectedClient* client,
                     C4Slice docID,
                     C4Slice collectionID,
                     C4Slice unlessRevID,
                     bool asFleece,
                     C4ConnectedClientGetDocumentCallback callback,
                     void *context,
                     C4Error* outError) noexcept {
    try {
        auto res = client->getDoc(docID, collectionID, unlessRevID, asFleece);
        res.then([=](C4DocResponse response) {
            return callback(client, &response, nullptr, context);
        }).onError([=](C4Error err) {
            return callback(client, nullptr, &err, context);
        });
    } catchError(outError);
    return;
}

void c4client_start(C4ConnectedClient* client) noexcept {
    client->start();
}

void c4client_stop(C4ConnectedClient* client) noexcept {
    client->stop();
}

void c4client_free(C4ConnectedClient* client) noexcept {
    if (!client)
        return;
    release(client);
}

void c4client_updateDoc(C4ConnectedClient* client,
                        C4Slice docID,
                        C4Slice collectionID,
                        C4Slice revID,
                        C4RevisionFlags revisionFlags,
                        C4Slice fleeceData,
                        C4ConnectedClientUpdateDocumentCallback callback,
                        void * C4NULLABLE context,
                        C4Error* outError) noexcept {
    try {
        bool deletion = (revisionFlags & kRevDeleted) != 0;
        revidBuffer generatedRev = DocumentFactory::generateDocRevID(fleeceData,
                                                                     revID,
                                                                     deletion);
        alloc_slice newRevID = revid(generatedRev).expanded();
        auto res = client->updateDoc(docID,
                                     collectionID,
                                     newRevID,
                                     revID,
                                     revisionFlags,
                                     fleeceData);
        res.then([=](C4Error err) {
            return callback(client, newRevID, &err, context);
        });
    } catchError(outError);
    return;
    
    
}

