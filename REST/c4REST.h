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

    typedef struct C4RESTListener C4RESTListener;

    C4RESTListener* c4rest_start(uint16_t port, C4Error *error) C4API;

    void c4rest_free(C4RESTListener *listener) C4API;

    void c4rest_shareDB(C4RESTListener *listener, C4String name, C4Database *db) C4API;

#ifdef __cplusplus
}
#endif
