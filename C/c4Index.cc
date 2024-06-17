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
    C4IndexImpl(C4Collection* c, IndexSpec spec) : C4Index(c, spec.name), _spec(std::move(spec)) {}

    C4IndexType getType() const noexcept { return C4IndexType(_spec.type); }

    C4QueryLanguage getQueryLanguage() const noexcept { return C4QueryLanguage(_spec.queryLanguage); }

    slice getExpression() const noexcept { return _spec.expression; }

    bool getOptions(C4IndexOptions& opts) const noexcept {
        opts = {};
        if ( auto ftsOpts = _spec.ftsOptions() ) {
            opts.language         = ftsOpts->language;
            opts.ignoreDiacritics = ftsOpts->ignoreDiacritics;
            opts.disableStemming  = ftsOpts->disableStemming;
            opts.stopWords        = ftsOpts->stopWords;
            return true;

#ifdef COUCHBASE_ENTERPRISE
        } else if ( auto vecOpts = _spec.vectorOptions() ) {
            opts.vector.dimensions      = vecOpts->dimensions;
            opts.vector.metric          = C4VectorMetricType(int(vecOpts->metric) + 1);
            opts.vector.clustering.type = C4VectorClusteringType(vecOpts->clusteringType());
            switch ( vecOpts->clusteringType() ) {
                case vectorsearch::ClusteringType::Flat:
                    {
                        auto flat = std::get<vectorsearch::FlatClustering>(vecOpts->clustering);
                        opts.vector.clustering.flat_centroids = flat.numCentroids;
                        break;
                    }
                case vectorsearch::ClusteringType::MultiIndex:
                    {
                        auto multi = std::get<vectorsearch::MultiIndexClustering>(vecOpts->clustering);
                        opts.vector.clustering.multi_bits          = multi.bitsPerSub;
                        opts.vector.clustering.multi_subquantizers = multi.subquantizers;
                        break;
                    }
            }
            opts.vector.encoding.type = C4VectorEncodingType(vecOpts->encodingType());
            switch ( vecOpts->encodingType() ) {
                case vectorsearch::EncodingType::None:
                    break;
                case vectorsearch::EncodingType::PQ:
                    {
                        auto pq                               = std::get<vectorsearch::PQEncoding>(vecOpts->encoding);
                        opts.vector.encoding.pq_subquantizers = pq.subquantizers;
                        opts.vector.encoding.bits             = pq.bitsPerSub;
                        break;
                    }
                case vectorsearch::EncodingType::SQ:
                    {
                        auto sq                   = std::get<vectorsearch::SQEncoding>(vecOpts->encoding);
                        opts.vector.encoding.bits = sq.bitsPerDimension;
                        break;
                    }
            }
            if ( vecOpts->probeCount ) opts.vector.numProbes = *vecOpts->probeCount;
            if ( vecOpts->minTrainingCount ) opts.vector.minTrainingSize = unsigned(*vecOpts->minTrainingCount);
            if ( vecOpts->maxTrainingCount ) opts.vector.maxTrainingSize = unsigned(*vecOpts->maxTrainingCount);
            opts.vector.lazy = vecOpts->lazyEmbedding;
            return true;
#endif
        } else {
            return false;
        }
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

    IndexSpec                     _spec;
    Retained<litecore::LazyIndex> _lazy;
};

inline C4IndexImpl* asInternal(C4Index* i) { return static_cast<C4IndexImpl*>(i); }

inline C4IndexImpl const* asInternal(C4Index const* i) { return static_cast<C4IndexImpl const*>(i); }

/*static*/ Retained<C4Index> C4Index::getIndex(C4Collection* c, slice name) {
    if ( optional<IndexSpec> spec = asInternal(c)->keyStore().getIndex(name) ) {
        return new C4IndexImpl(c, *std::move(spec));
    } else {
        return nullptr;
    }
}

C4IndexType C4Index::getType() const noexcept { return asInternal(this)->getType(); }

C4QueryLanguage C4Index::getQueryLanguage() const noexcept { return asInternal(this)->getQueryLanguage(); }

slice C4Index::getExpression() const noexcept { return asInternal(this)->getExpression(); }

bool C4Index::getOptions(C4IndexOptions& opts) const noexcept { return asInternal(this)->getOptions(opts); }


#ifdef COUCHBASE_ENTERPRISE

bool C4Index::isTrained() const { return _collection->isIndexTrained(_name); }

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
    // Invariants: _update != nullptr || (finish() has been called)
    if ( !_update ) {
        litecore::error::_throw(litecore::error::NotOpen, "finish() has been already been called on this updater.");
    }
    auto                    db = _collection->getDatabase();
    C4Database::Transaction txn(db);
    bool                    done = _update->finish(asInternal(db)->transaction());
    txn.commit();
    _update     = nullptr;
    _collection = nullptr;
    return done;
}

#endif
