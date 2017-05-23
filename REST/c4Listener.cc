//
//  c4Listener.cc
//  LiteCore
//
//  Created by Jens Alfke on 5/22/17.
//  Copyright © 2017 Couchbase. All rights reserved.
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
        alloc_slice result(name);
        result.retain();
        return {(char*)result.buf, result.size};
    } catchExceptions()
    return {};
}


bool c4listener_shareDB(C4Listener *listener, C4String name, C4Database *db) noexcept {
    try {
        return internal(listener)->registerDatabase(slice(name).asString(), db);
    } catchExceptions()
    return false;
}


bool c4listener_unshareDB(C4Listener *listener, C4String name) noexcept {
    try {
        return internal(listener)->unregisterDatabase(slice(name).asString());
    } catchExceptions()
    return false;
}
