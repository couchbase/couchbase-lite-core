//
//  Error.hh
//  CBForest
//
//  Created by Jens Alfke on 6/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef CBForest_Error_h
#define CBForest_Error_h

#include "forestdb.h"

namespace forestdb {

    // Most API calls can throw this
    struct error {
        enum CBForestError {
            BadRevisionID = -1000,
            CorruptRevisionData = -1001,
        };

        int status;
        error (fdb_status s)        :status(s) {}
        error (CBForestError e)     :status(e) {}
    };

}

#endif
