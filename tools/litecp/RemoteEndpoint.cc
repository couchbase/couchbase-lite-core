//
//  RemoteEndpoint.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/26/17.
//Copyright © 2017 Couchbase. All rights reserved.
//

#include "RemoteEndpoint.hh"
#include "DBEndpoint.hh"


void RemoteEndpoint::prepare(bool isSource, bool mustExist, slice docIDProperty, const Endpoint *other) {
    Endpoint::prepare(isSource, mustExist, docIDProperty, other);

    if (!c4repl_parseURL(slice(_spec), &_address, &_dbName))
        fail("Invalid database URL");
}


// As source (i.e. pull):
void RemoteEndpoint::copyTo(Endpoint *dst, uint64_t limit) {
    auto dstDB = dynamic_cast<DbEndpoint*>(dst);
    if (dstDB)
        dstDB->replicateWith(*this, kC4Disabled, kC4OneShot);
    else
        fail("Sorry, this mode isn't supported.");
}


// As destination:
void RemoteEndpoint::writeJSON(slice docID, slice json) {
    fail("Sorry, this mode isn't supported.");
}


void RemoteEndpoint::finish() {
}
