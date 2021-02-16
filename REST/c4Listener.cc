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
#include "fleece/Mutable.hh"
#include <algorithm>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::REST;


namespace litecore { namespace REST {
    C4LogDomain ListenerLog;
} }


static inline RESTListener* internal(C4Listener* r) {return (RESTListener*)r;}
static inline C4Listener* external(Listener* r) {return (C4Listener*)r;}


C4ListenerAPIs c4listener_availableAPIs(void) C4API {
    return kListenerAPIs;
}


C4Listener* c4listener_start(const C4ListenerConfig *config, C4Error *outError) C4API {
    C4Listener* c4Listener = nullptr;
    try {
        c4Listener = external(retain(NewListener(config)));
        if (!c4Listener)
            c4error_return(LiteCoreDomain, kC4ErrorUnsupported,
                           "Unsupported listener API"_sl, outError);
    } catchError(outError)
    return c4Listener;
}


void c4listener_free(C4Listener *listener) C4API {
    if (!listener)
        return;
    internal(listener)->stop();
    release(internal(listener));
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
        c4error_return(LiteCoreDomain, kC4ErrorConflict, "Database already shared"_sl, outError);
    } catchError(outError);
    return false;
}


bool c4listener_unshareDB(C4Listener *listener, C4Database *db,
                          C4Error *outError) C4API
{
    try {
        if (internal(listener)->unregisterDatabase(db))
            return true;
        c4error_return(LiteCoreDomain, kC4ErrorNotOpen, "Database not shared"_sl, outError);
    } catchError(outError);
    return false;
}


uint16_t c4listener_getPort(C4Listener *listener) C4API {
    try {
        return internal(listener)->port();
    } catchExceptions()
    return 0;
}


FLMutableArray c4listener_getURLs(C4Listener *listener, C4Database *db,
                                  C4ListenerAPIs api, C4Error* err) C4API {
    try {
        switch(api) {
            case kC4RESTAPI:
            case kC4SyncAPI:
                break;
            default:
                if(err != nullptr) {
                    *err = c4error_make(LiteCoreDomain, kC4ErrorInvalidParameter,
                                        C4STR("The provided API must be one of the following:  REST, Sync."));
                }
                
                return nullptr;
        }
        
        MutableArray urls = MutableArray::newArray();
        for (net::Address &address : internal(listener)->addresses(db, api))
            urls.append(address.url());
        return (FLMutableArray)FLValue_Retain(urls);
    } catchError(err);
    return nullptr;
}


void c4listener_getConnectionStatus(C4Listener *listener,
                                    unsigned *connectionCount,
                                    unsigned *activeConnectionCount) C4API
{
    auto active = internal(listener)->activeConnectionCount();
    if (connectionCount)
        *connectionCount = std::max(internal(listener)->connectionCount(), active);
    if (activeConnectionCount)
        *activeConnectionCount = active;
    // Ensure activeConnectionCount ≤ connectionCount because logically it should be;
    // sometimes it isn't, because the TCP connection has closed but the Replicator instance
    // is still finishing up. In that case, bump the connectionCount accordingly.
}
