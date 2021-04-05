//
// REST_CAPI.cc
//
// Copyright © 2021 Couchbase. All rights reserved.
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

#include "c4Listener.h"
#include "c4Listener.hh"
#include "c4ExceptionUtils.hh"
#include "fleece/Mutable.hh"

using namespace std;
using namespace fleece;
using namespace litecore;


C4ListenerAPIs c4listener_availableAPIs(void) noexcept {
    return C4Listener::availableAPIs();
}


C4Listener* c4listener_start(const C4ListenerConfig *config, C4Error *outError) noexcept {
    try {
        return new C4Listener(*config);
    } catchError(outError)
    return nullptr;
}


void c4listener_free(C4Listener *listener) noexcept {
    delete listener;
}


C4StringResult c4db_URINameFromPath(C4String pathSlice) noexcept {
    try {
        if (string name = C4Listener::URLNameFromPath(pathSlice); name.empty())
        return {};
        else
            return FLSliceResult(alloc_slice(name));
    } catchAndWarn()
    return {};
}


bool c4listener_shareDB(C4Listener *listener, C4String name, C4Database *db,
                        C4Error *outError) noexcept
{
    try {
        return listener->shareDB(name, db);
    } catchError(outError);
    return false;
}


bool c4listener_unshareDB(C4Listener *listener, C4Database *db,
                          C4Error *outError) noexcept
{
    try {
        if (listener->unshareDB(db))
            return true;
        c4error_return(LiteCoreDomain, kC4ErrorNotOpen, "Database not shared"_sl, outError);
    } catchError(outError);
    return false;
}


uint16_t c4listener_getPort(C4Listener *listener) noexcept {
    try {
        return listener->port();
    } catchAndWarn()
    return 0;
}


FLMutableArray c4listener_getURLs(C4Listener *listener, C4Database *db,
                                  C4ListenerAPIs api, C4Error* err) noexcept {
    try {
        auto urls = fleece::MutableArray::newArray();
        for (string &url : listener->URLs(db, api))
            urls.append(url);
        return (FLMutableArray)FLValue_Retain(urls);
    } catchError(err);
    return nullptr;
}


void c4listener_getConnectionStatus(C4Listener *listener,
                                    unsigned *connectionCount,
                                    unsigned *activeConnectionCount) noexcept
{
    auto [conns, active] = listener->connectionStatus();
    if (connectionCount)
        *connectionCount = conns;
    if (activeConnectionCount)
        *activeConnectionCount = active;
}
