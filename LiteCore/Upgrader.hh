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

    /** Upgrades a 1.x database in place; afterwards it will be a current database.
        The database MUST NOT be open by any other connections.
        Returns false if the configuration does not allow for upgrading the database. */
    bool UpgradeDatabaseInPlace(const FilePath &path, C4DatabaseConfig);
    
}
