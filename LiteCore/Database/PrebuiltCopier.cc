//
//  PrebuiltCopier.cpp
//  LiteCore
//
//  Created by Jim Borden on 2017/08/15.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "PrebuiltCopier.hh"
#include "StringUtil.hh"
#include "Error.hh"
#include <thread>

namespace litecore {
    bool CopyPrebuiltDB(const litecore::FilePath &from, const litecore::FilePath &to,
                             const C4DatabaseConfig *config, C4Error* outError) {
        if(!from.exists()) {
            Warn("No database exists at %s, cannot copy!", from.path().data());
            outError->code = ENOENT;
            outError->domain = POSIXDomain;
            return false;
        }
        
        FilePath backupPath;
        Log("Copying prebuilt database from %s to %s", from.path().data(), to.path().data());
        bool needBackup = to.exists();
        
        if(needBackup) {
            
        }
        
        try {
            FilePath temp = FilePath::tempDirectory().mkTempDir();
            temp.delRecursive();
            from.copyTo(temp);
            
            auto db = c4db_open(c4str(temp.path().data()), config, outError);
            if(db) {
                Log("Resetting UUIDs...");
                db->resetUUIDs();
                c4db_free(db);
                
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
                
                Log("Moving source DB to destination DB...");
                temp.moveTo(to);
                Log("Finished, asynchronously deleting backup");
                thread( [=]{
                    backupPath.delRecursive();
                    Log("Copier finished async delete of backup db at %s", backupPath.path().c_str());
                } ).detach();
                
                return true;
            }
        } catchError(outError)
        
        try {
            Warn("Failed to finish copying database (%s)",
                 error::_what((error::Domain)outError->domain, outError->code).data());
            Database::deleteDatabaseAtPath(to.path(), config);
            if(needBackup) {
                if(backupPath.exists()) {
                    backupPath.moveTo(to);
                } else {
                    WarnError("The backup of the database has vanished. \
                              This should be reported immediately");
                }
            }
            
            return false;
        } catchError(outError)
        
        WarnError("An error occurred during the rollback process (%s)",
                  error::_what((error::Domain)outError->domain, outError->code).data());
        
        return false;
    }
}
