//
// Upgrader.hh
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
