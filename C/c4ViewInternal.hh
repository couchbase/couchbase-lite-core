//
//  c4ViewInternal.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/16/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "c4Internal.hh"
#include "Database.hh"
#include "MapReduceIndex.hh"


struct c4View : public RefCounted<c4View> {
    c4View(Database *sourceDB,
           const FilePath &path,
           C4Slice viewName,
           C4Slice version,
           const C4DatabaseConfig &config)
    :_sourceDB(sourceDB),
     _viewDB(Database::newDataFile(path, config, false)),
     _index(_viewDB->getKeyStore((string)viewName), *sourceDB->dataFile())
    {
        setVersion(version);
    }

    void setVersion(C4Slice version) {
        _index.setup(-1, (string)version);
    }

    bool checkNotBusy(C4Error *outError) {
        if (_index.isBusy()) {
            recordError(LiteCoreDomain, kC4ErrorIndexBusy, outError);
            return false;
        }
        return true;
    }

    void close() {
        _viewDB->close();
    }

    Retained<Database> _sourceDB;
    unique_ptr<DataFile> _viewDB;
    MapReduceIndex _index;
#if C4DB_THREADSAFE
    mutex _mutex;
#endif
};
