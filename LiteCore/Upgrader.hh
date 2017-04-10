//
//  Upgrader.hh
//  LiteCore
//
//  Created by Jens Alfke on 4/9/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "FilePath.hh"
#include "Database.hh"
#include "c4Database.h"

namespace litecore {

    /** Reads a Couchbase Lite 1.x (where x >= 2) SQLite database into a new database. */
    void UpgradeDatabase(const FilePath &oldPath, const FilePath &newPath, C4DatabaseConfig);

    /** Reads a Couchbase Lite 1.x (where x >= 2) SQLite database into a new open database. */
    void UpgradeDatabase(const FilePath &oldPath, Database *newDB);
    
}
