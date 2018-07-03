//
// PrebuiltCopier.cc
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

    static void createLocalCheckpointDocument(const unique_ptr<C4Database> &db)
    {
        C4UUID privateUUID;
        C4Error err;
        if(!c4db_getUUIDs(db.get(), nullptr, &privateUUID, &err)) {
            error::_throw(error::Domain::LiteCore, err.code);
        }

        fleeceapi::Encoder enc;
        enc.beginDict(1);
        enc.writeKey(constants::kLocalCheckpointLocalUUID);
        enc.writeData({&privateUUID, sizeof(C4UUID)});
        enc.endDict();
        FLError flErr;
        const FLSliceResult body = enc.finish(&flErr);
        if(body.size == 0) {
            error::_throw(error::Domain::Fleece, flErr);
        }

        const bool success = c4raw_put(db.get(), constants::kLocalCheckpointStore, constants::kLocalCheckpointDocID, nullslice, C4Slice(body), &err);
        FLSliceResult_Free(body);
        if(!success) {
            error::_throw(error::Domain::LiteCore, err.code);
        }
    }
    
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
        
        auto db = make_unique<C4Database>(temp.path(), *config);
        createLocalCheckpointDocument(db);
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
