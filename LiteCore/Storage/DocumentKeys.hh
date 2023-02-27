//
// DocumentKeys.hh
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Base.hh"
#include "DataFile.hh"
#include "Record.hh"
#include "SharedKeys.hh"

namespace litecore {
    using namespace fleece;

    /** SharedKeys implementation that stores the keys in a DataFile. */
    class DocumentKeys final : public fleece::impl::PersistentSharedKeys {
      public:
        DocumentKeys(DataFile &db)
            : _db(db), _keyStore(_db.getKeyStore(DataFile::kInfoKeyStoreName, KeyStore::noSequences)) {}

      protected:
        virtual bool read() override {
            Record r = _keyStore.get("SharedKeys"_sl);
            return loadFrom(r.body());
        }

        virtual void write(slice encodedData) override {
            _keyStore.setKV("SharedKeys"_sl, encodedData, _db.transaction());
        }

      private:
        DataFile &_db;
        KeyStore &_keyStore;
    };

}  // namespace litecore
