//
//  c4REST.h
//  LiteCore
//
//  Created by Jens Alfke on 4/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "c4Database.h"

#ifdef __cplusplus
extern "C" {
#endif

    /** Filename extension of databases -- ".cblite2". Note that it includes the dot. */
    extern const char* const kC4DatabaseFilenameExtension;

    typedef struct C4RESTConfig {
        uint16_t port;
        C4String directory;
        bool allowCreateDBs;
        bool allowDeleteDBs;
    } C4RESTConfig;

    typedef struct C4RESTListener C4RESTListener;

    C4RESTListener* c4rest_start(C4RESTConfig *config, C4Error *error) C4API;

    void c4rest_free(C4RESTListener *listener) C4API;

    C4StringResult c4rest_databaseNameFromPath(C4String path) C4API;

    void c4rest_shareDB(C4RESTListener *listener, C4String name, C4Database *db) C4API;

#ifdef __cplusplus
}
#endif
