//
//  PrebuiltCopier.cc
//  LiteCore
//
//  Created by Jim Borden on 2017/08/15.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "PrebuiltCopier.hh"
#include "Logging.hh"
#include "FilePath.hh"
#include "Database.hh"
#include "StringUtil.hh"
#include "Error.hh"
#include "c4Database.h"
#include <future>

namespace litecore {
    using namespace std;
    
    void CopyPrebuiltDB(const litecore::FilePath &from, const litecore::FilePath &to,
                             const C4DatabaseConfig *config) {
        if(!from.exists()) {
            Warn("No database exists at %s, cannot copy!", from.path().c_str());
            error::_throw(error::Domain::LiteCore, C4ErrorCode::kC4ErrorNotFound);
        }
        
        FilePath backupPath;
        Log("Copying prebuilt database from %s to %s", from.path().c_str(), to.path().c_str());
        bool needBackup = to.exists();

        FilePath temp = FilePath::tempDirectory().mkTempDir();
        temp.delRecursive();
        from.copyTo(temp);
        
        auto db = unique_ptr<C4Database>(new C4Database(temp.path(), *config));
        db->resetUUIDs();
        db->close();
        if(needBackup) {
            Log("Backing up destination DB...");
            
            // TODO: Have a way to restore these temp backups if a crash happens?
            string p = to.path();
            chomp(p, '/');
            chomp(p, '\\');
            backupPath = FilePath(p + "_TEMP/");
            backupPath.delRecursive();
            to.moveTo(backupPath);
        }
        
        try {
            Log("Moving source DB to destination DB...");
            temp.moveTo(to);
        } catch(exception &) {
            Warn("Failed to finish copying database");
            to.delRecursive();
            if(needBackup) {
                if(backupPath.exists()) {
                    backupPath.moveTo(to);
                } else {
                    WarnError("The backup of the database has vanished. \
                              This should be reported immediately");
                }
            }
            throw;
        }
        
        Log("Finished, asynchronously deleting backup");
        async([=]{
            try {
                backupPath.delRecursive();
                Log("Copier finished async delete of backup db at %s", backupPath.path().c_str());
            } catch(exception &x) {
                Warn("Error deleting database backup (%s)", x.what());
            }
        });
    }
}
