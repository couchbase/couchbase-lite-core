//
// RemoteEndpoint.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include "RemoteEndpoint.hh"
#include "DBEndpoint.hh"


void RemoteEndpoint::prepare(bool isSource, bool mustExist, slice docIDProperty, const Endpoint *other) {
    Endpoint::prepare(isSource, mustExist, docIDProperty, other);

    if (!c4repl_parseURL(slice(_spec), &_address, &_dbName))
        Tool::instance->fail("Invalid database URL");
}


// As source (i.e. pull):
void RemoteEndpoint::copyTo(Endpoint *dst, uint64_t limit) {
    auto dstDB = dynamic_cast<DbEndpoint*>(dst);
    if (dstDB)
        dstDB->replicateWith(*this, kC4Disabled, kC4OneShot);
    else
        Tool::instance->fail("Sorry, this mode isn't supported.");
}


// As destination:
void RemoteEndpoint::writeJSON(slice docID, slice json) {
    Tool::instance->fail("Sorry, this mode isn't supported.");
}


void RemoteEndpoint::finish() {
}
