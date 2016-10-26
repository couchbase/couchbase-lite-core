//
//  DocumentKeys.hh
//  LiteCore
//
//  Created by Jens Alfke on 10/24/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"
#include "DataFile.hh"
#include "Record.hh"
#include "SharedKeys.hh"

namespace litecore {
    using namespace fleece;


    /** SharedKeys implementation that stores the keys in a DataFile. */
    class DocumentKeys : public fleece::PersistentSharedKeys {
    public:
        DocumentKeys(DataFile &db)
        :_db(db),
        _keyStore(_db.getKeyStore(DataFile::kInfoKeyStoreName))
        { }

    protected:
        virtual bool read() override {
            Record r = _keyStore.get("SharedKeys"_sl);
            return loadFrom(r.body());
        }
        virtual void write(slice encodedData) override {
            _keyStore.set("SharedKeys"_sl, nullslice, encodedData, _db.transaction());
        }

    private:
        DataFile &_db;
        KeyStore &_keyStore;
    };

}
