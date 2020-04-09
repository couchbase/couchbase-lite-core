//
// c4Listener.cc
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

#include "c4.hh"
#include "c4Listener.h"
#include "c4Internal.hh"
#include "c4ListenerInternal.hh"
#include "c4ExceptionUtils.hh"
#include "Listener.hh"
#include "RESTListener.hh"

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::REST;


namespace litecore { namespace REST {
    C4LogDomain RESTLog;
} }


static inline RESTListener* internal(C4Listener* r) {return (RESTListener*)r;}
static inline C4Listener* external(Listener* r) {return (C4Listener*)r;}


C4ListenerAPIs c4listener_availableAPIs(void) C4API {
    return kListenerAPIs;
}


C4Listener* c4listener_start(const C4ListenerConfig *config, C4Error *outError) C4API {
    C4Listener* listener = nullptr;
    try {
        listener = external(NewListener(config));
        if (!listener)
            c4error_return(LiteCoreDomain, kC4ErrorUnsupported,
                           "Unsupported listener API"_sl, outError);
    } catchError(outError)
    return listener;
}


void c4listener_free(C4Listener *listener) C4API {
    delete internal(listener);
}


C4StringResult c4db_URINameFromPath(C4String pathSlice) C4API {
    try {
        auto pathStr = slice(pathSlice).asString();
        string name = Listener::databaseNameFromPath(FilePath(pathStr, ""));
        if (name.empty())
            return {};
        return FLSliceResult(alloc_slice(name));
    } catchExceptions()
    return {};
}


bool c4listener_shareDB(C4Listener *listener, C4String name, C4Database *db,
                        C4Error *outError) C4API
{
    try {
        optional<string> nameStr;
        if (name.buf)
            nameStr = slice(name);
        if (internal(listener)->registerDatabase(db, nameStr))
            return true;
        recordError(LiteCoreDomain, kC4ErrorConflict, "Database already shared", outError);
    } catchError(outError);
    return false;
}


bool c4listener_unshareDB(C4Listener *listener, C4Database *db,
                          C4Error *outError) C4API
{
    try {
        if (internal(listener)->unregisterDatabase(db))
            return true;
        recordError(LiteCoreDomain, kC4ErrorNotOpen, "Database not shared", outError);
    } catchError(outError);
    return false;
}


uint16_t c4listener_getPort(C4Listener *listener) C4API {
    try {
        return internal(listener)->port();
    } catchExceptions()
    return 0;
}


C4StringResult c4listener_getURLs(C4Listener *listener, C4Database *db) C4API {
    try {
        stringstream out;
        int n = 0;
        for (net::Address &address : internal(listener)->addresses(db)) {
            if (n++ > 0) out << "\n";
            out << address.url();
        }
        alloc_slice result(out.str());
        return C4StringResult(result);
    } catchExceptions()
    return {};
}
