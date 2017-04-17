//
//  c4REST.h
//  LiteCore
//
//  Created by Jens Alfke on 4/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "c4Base.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct C4RESTListener C4RESTListener;

    C4RESTListener* c4rest_start(uint16_t port, C4Error *error);

    void c4rest_free(C4RESTListener*);

#ifdef __cplusplus
}
#endif
