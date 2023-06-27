//
// PrebuiltCopier.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "PrebuiltCopier.hh"
#include "c4Database.hh"
#include "DatabaseImpl.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include <future>

namespace litecore {
    using namespace std;

    void CopyPrebuiltDB(const litecore::FilePath& from, const litecore::FilePath& to, const C4DatabaseConfig* config) {
        if ( !from.exists() ) {
            Warn("No database exists at %s, cannot copy!", from.path().c_str());
            error::_throw(error::Domain::LiteCore, kC4ErrorNotFound);
        }

        if ( to.exists() ) {
            Warn("Database already exists at %s, cannot copy!", to.path().c_str());
            error::_throw(error::Domain::POSIX, EEXIST);
        }

        FilePath backupPath;
        Log("Copying prebuilt database from %s to %s", from.path().c_str(), to.path().c_str());

        FilePath temp = FilePath::sharedTempDirectory(to.parentDir()).mkTempDir();
        temp.delRecursive();
        from.copyTo(temp);

        {
            Retained<C4Database> db;
            try {
                db = C4Database::openAtPath(temp.path(), config->flags, &config->encryptionKey);
            } catch ( const error& x ) {
                if ( x.domain == error::LiteCore && x.code == error::NotADatabaseFile ) {
                    Warn("Cannot open the copied database with the given encryption key. "
                         "The given encryption key needs to be matched with the encryption key "
                         "of the original database. To change the encryption key, open the copied "
                         "database then change the encryption key.");
                }
                throw;
            }
            asInternal(db)->resetUUIDs();
            db->close();
        }

        try {
            Log("Moving source DB to destination DB...");
            temp.moveTo(to);
        } catch ( ... ) {
            Warn("Failed to finish copying database");
            to.delRecursive();
            throw;
        }
    }
}  // namespace litecore
