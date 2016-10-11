//
//  ForestDBMain.c
//  LiteCore
//
//  Created by Jens Alfke on 10/11/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "forestdb.h"

void foo(void);

void foo(void) {
    fdb_init(NULL);
}
