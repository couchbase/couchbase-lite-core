//
// DocumentKeys.hh
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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
        :_db(db),
        _keyStore(_db.getKeyStore(DataFile::kInfoKeyStoreName, KeyStore::noSequences))
        { }

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

}
