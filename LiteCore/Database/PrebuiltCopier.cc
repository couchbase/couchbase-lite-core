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

        if (to.exists()) {
            Warn("Database already exists at %s, cannot copy!", to.path().c_str());
            error::_throw(error::Domain::POSIX, EEXIST);
        }
        
        FilePath backupPath;
        Log("Copying prebuilt database from %s to %s", from.path().c_str(), to.path().c_str());

        FilePath temp = FilePath::tempDirectory().mkTempDir();
        temp.delRecursive();
        from.copyTo(temp);
        
        auto db = unique_ptr<C4Database>(new C4Database(temp.path(), *config));
        db->resetUUIDs();
        db->close();
        
        try {
            Log("Moving source DB to destination DB...");
            temp.moveTo(to);
        } catch(...) {
            Warn("Failed to finish copying database");
            to.delRecursive();
            throw;
        }
    }
}
