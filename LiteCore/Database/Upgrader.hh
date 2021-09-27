//
// Upgrader.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "FilePath.hh"
#include "DatabaseImpl.hh"
#include "c4DatabaseTypes.h"

namespace litecore {

    /** Reads a Couchbase Lite 1.x (where x >= 2) SQLite database into a new database. */
    void UpgradeDatabase(const FilePath &oldPath, const FilePath &newPath, C4DatabaseConfig);

    /** Upgrades a 1.x database in place; afterwards it will be a current database.
        The database MUST NOT be open by any other connections.
        Returns false if the configuration does not allow for upgrading the database. */
    bool UpgradeDatabaseInPlace(const FilePath &path, C4DatabaseConfig);
    
}
