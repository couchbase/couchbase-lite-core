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


static inline Listener* internal(C4Listener* r) {return (Listener*)r;}
static inline C4Listener* external(Listener* r) {return (C4Listener*)r;}


C4ListenerAPIs c4listener_availableAPIs(void) noexcept {
    return kListenerAPIs;
}


C4Listener* c4listener_start(const C4ListenerConfig *config, C4Error *outError) noexcept {
    C4Listener* listener = nullptr;
    try {
        listener = external(NewListener(config));
        if (!listener)
            c4error_return(LiteCoreDomain, kC4ErrorUnsupported,
                           "Unsupported listener API"_sl, outError);
    } catchError(outError)
    return listener;
}


void c4listener_free(C4Listener *listener) noexcept {
    delete internal(listener);
}


C4StringResult c4db_URINameFromPath(C4String pathSlice) noexcept {
    try {
        auto pathStr = slice(pathSlice).asString();
        string name = Listener::databaseNameFromPath(FilePath(pathStr, ""));
        if (name.empty())
            return {};
        return FLSliceResult(alloc_slice(name));
    } catchExceptions()
    return {};
}


bool c4listener_shareDB(C4Listener *listener, C4String name, C4Database *db) noexcept {
    try {
        optional<string> nameStr;
        if (name.buf)
            nameStr = slice(name);
        return internal(listener)->registerDatabase(db, nameStr);
    } catchExceptions()
    return false;
}


bool c4listener_unshareDB(C4Listener *listener, C4Database *db) noexcept {
    try {
        return internal(listener)->unregisterDatabase(db);
    } catchExceptions()
    return false;
}


C4StringResult c4listener_getURL(C4Listener *listener, C4Database *db) C4API {
    try {
        return C4StringResult( ((RESTListener*)internal(listener))->address(db).url() );
    } catchExceptions()
    return {};
}
