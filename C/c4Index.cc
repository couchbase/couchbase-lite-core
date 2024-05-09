//
// c4Index.cc
//
// Copyright © 2024 Couchbase. All rights reserved.
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

#include "c4Index.hh"

#include "CollectionImpl.hh"
#include "DatabaseImpl.hh"
#include "LazyIndex.hh"


using namespace std;
using namespace fleece;
using namespace litecore;

struct C4IndexImpl final : public C4Index {
    C4IndexImpl(C4Collection* c, slice name) : _spec(asInternal(c)->keyStore().getIndex(name)) {
        _collection = c;
        _name       = name;
    }

#ifdef COUCHBASE_ENTERPRISE
    Retained<C4IndexUpdater> beginUpdate(size_t limit) {
        if ( !_lazy ) _lazy = new LazyIndex(asInternal(_collection)->keyStore(), _name);
        Retained<LazyIndexUpdate> update = _lazy->beginUpdate(limit);
        if ( update ) return new C4IndexUpdater(std::move(update), _collection);
        else
            return nullptr;
    }
#endif

    optional<IndexSpec>           _spec;
    Retained<litecore::LazyIndex> _lazy;
};

inline C4IndexImpl* asInternal(C4Index* index) { return static_cast<C4IndexImpl*>(index); }

Retained<C4Index> C4Index::getIndex(C4Collection* c, slice name) {
    Retained<C4IndexImpl> index = new C4IndexImpl(c, name);
    if ( !index->_spec ) index = nullptr;
    return index;
}


#ifdef COUCHBASE_ENTERPRISE

Retained<C4IndexUpdater> C4Index::beginUpdate(size_t limit) { return asInternal(this)->beginUpdate(limit); }

C4IndexUpdater::C4IndexUpdater(Retained<litecore::LazyIndexUpdate> u, C4Collection* c)
    : _update(std::move(u)), _collection(c) {}

C4IndexUpdater::~C4IndexUpdater() = default;

size_t C4IndexUpdater::count() const { return _update->count(); }

FLValue C4IndexUpdater::valueAt(size_t i) const { return _update->valueAt(i); }

void C4IndexUpdater::setVectorAt(size_t i, const float* vector, size_t dimension) {
    _update->setVectorAt(i, vector, dimension);
}

void C4IndexUpdater::skipVectorAt(size_t i) { return _update->skipVectorAt(i); }

bool C4IndexUpdater::finish() {
    auto                    db = _collection->getDatabase();
    C4Database::Transaction txn(db);
    bool                    done = _update->finish(asInternal(db)->transaction());
    txn.commit();
    _update     = nullptr;
    _collection = nullptr;
    return done;
}

#endif
