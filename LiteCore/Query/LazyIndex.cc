//
// LazyIndexUpdate.cc
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

#ifdef COUCHBASE_ENTERPRISE

#    include "LazyIndex.hh"
#    include "BothKeyStore.hh"
#    include "Error.hh"
#    include "Query.hh"
#    include "SequenceSet.hh"
#    include "SQLiteDataFile.hh"
#    include "SQLite_Internal.hh"
#    include "SQLiteKeyStore.hh"
#    include "SQLUtil.hh"
#    include "SQLite_Internal.hh"
#    include "StringUtil.hh"

#    include "Array.hh"  // fleece::internal
#    include "fleece/Fleece.hh"
#    include "SQLiteCpp/SQLiteCpp.h"

namespace litecore {
    using namespace std;

    // Names for the result columns in the Query
    enum { kRowIDCol, kSequenceCol, kValueCol };

#    pragma mark - LAZY INDEX MANAGER:

    LazyIndex::LazyIndex(KeyStore& keyStore, string_view indexName)
        : _keyStore(keyStore)
        , _indexName(indexName)
        , _db(dynamic_cast<SQLiteDataFile&>(keyStore.dataFile()))
        , _sqlKeyStore(*_db.asSQLiteKeyStore(&keyStore)) {
        SQLiteIndexSpec spec = this->getSpec();
        _vectorTableName     = spec.indexTableName;

        // JSON query that will select the rowid and input value of up to $limit new/changed docs,
        // in sequence order:
        auto   exprJson  = Array(FLArray(spec.what()))[0].toJSONString();
        string queryJson = R"(["SELECT", {
                                "WHAT": [ ["._rowID"], ["._sequence"], )"
                           + exprJson + R"( ],
                                "WHERE": ["AND", [">=", ["._sequence"], ["$startSeq"]],
                                            ["IS NOT", )"
                           + exprJson + R"(, ["MISSING"]] ],
                                "ORDER_BY": [ ["._sequence"] ],
                                "LIMIT": ["$limit"]}])";
        _query = _db.compileQuery(queryJson, QueryLanguage::kJSON, &keyStore);
    }

    SQLiteIndexSpec LazyIndex::getSpec() const {
        optional<SQLiteIndexSpec> spec = _db.getIndex(_indexName);
        if ( !spec || spec->keyStoreName != _sqlKeyStore.name() )
            error::_throw(error::NoSuchIndex, "No such index in collection");
        if ( !spec->indexedSequences ) error::_throw(error::UnsupportedOperation, "Index is not lazy");
        Assert(spec->type == IndexSpec::kVector);  // no other type supports laziness (yet)
        return *std::move(spec);
    }

    Retained<LazyIndexUpdate> LazyIndex::beginUpdate(size_t limit) {
        AssertArg(limit > 0);
        Retained<LazyIndexUpdate> update;
        do {
            unsigned    dimension = 0;
            SequenceSet indexedSequences;
            sequence_t  curSeq;
            {
                // Open a RO transaction so the code sees a consistent snapshot of the database:
                ReadOnlyTransaction txn(_db);

                {
                    SQLiteIndexSpec spec = getSpec();
                    if ( auto vecOpts = spec.vectorOptions() ) dimension = vecOpts->dimensions;
                    if ( !indexedSequences.read_json(spec.indexedSequences) )
                        LogError(QueryLog, "Couldn't parse index's indexedSequences: %.*s",
                                 FMTSLICE(spec.indexedSequences));
                }
                curSeq = _sqlKeyStore.lastSequence();
                LogTo(QueryLog, "LazyIndex: Indexed sequences of %s are %s ; latest seq is %llu", _indexName.c_str(),
                      indexedSequences.to_string().c_str(), (long long)curSeq);
                if ( indexedSequences.contains(sequence_t{1}, curSeq + 1) ) break;  // Index is up-to-date

                // Find the first missing sequence:
                sequence_t startSeq{1};
                if ( !indexedSequences.empty() && indexedSequences.begin()->first <= 1_seq )
                    startSeq = indexedSequences.begin()->second;

                Encoder enc;
                enc.beginDict();
                enc["startSeq"] = int64_t(startSeq);
                enc["limit"]    = limit;
                enc.endDict();
                Query::Options            options(enc.finish());
                Retained<QueryEnumerator> e = _query->createEnumerator(&options);
                if ( e->getRowCount() > 0 )
                    update = new LazyIndexUpdate(this, dimension, startSeq, curSeq, indexedSequences, e, limit);
            }

            if ( !update ) {
                // No vectors to index; mark index as up-to-date:
                indexedSequences.add(sequence_t{1}, curSeq + 1);
                updateIndexedSequences(indexedSequences);
                break;
            } else if ( update->count() == 0 ) {
                // No vectors for the caller to compute; finish the update now:
                ExclusiveTransaction txn(_db);
                update->finish(txn);
                txn.commit();
                // ...and repeat the loop to fetch `limit` more rows
                update = nullptr;
            }
        } while ( !update );
        return update;
    }

    void LazyIndex::insert(int64_t rowid, float vec[], size_t dimension) {
        if ( !_ins ) {
            _ins = make_unique<SQLite::Statement>(_db, CONCAT("INSERT OR REPLACE INTO "
                                                              << sqlIdentifier(_vectorTableName)
                                                              << " (docid, vector) VALUES (?1, ?2)"));
        }
        UsingStatement u(_ins);
        _ins->bind(1, (long long)rowid);
        _ins->bindNoCopy(2, (const void*)vec, int(dimension * sizeof(float)));
        _ins->exec();
    }

    void LazyIndex::del(int64_t rowid) {
        if ( !_del ) {
            _del = make_unique<SQLite::Statement>(
                    _db, CONCAT("DELETE FROM " << sqlIdentifier(_vectorTableName) << " WHERE docid=?1"));
        }
        UsingStatement u(_del);
        _del->bind(1, (long long)rowid);
        _del->exec();
    }

    void LazyIndex::updateIndexedSequences(SequenceSet const& seq) {
        LogTo(QueryLog, "LazyIndex: Updating %s indexed sequences to %s", _indexName.c_str(), seq.to_string().c_str());
        _db.setIndexSequences(_indexName, seq.to_json());
    }

#    pragma mark - LAZY INDEX UPDATE:

    LazyIndexUpdate::LazyIndexUpdate(LazyIndex* manager, unsigned dimension, sequence_t firstSeq, sequence_t atSeq,
                                     SequenceSet indexedSeqs, Retained<QueryEnumerator> e, size_t limit)
        : _manager(manager)
        , _firstSeq(firstSeq)
        , _atSeq(atSeq)
        , _indexedSequences(std::move(indexedSeqs))
        , _enum(std::move(e))
        , _dimension(dimension) {
        // Find the rows which are not yet indexed:
        int64_t row = 0;
        for ( ; _enum->next(); ++row ) {
            _lastSeq = sequence_t{_enum->columns()[kSequenceCol]->asUnsigned()};
            if ( !_enum->isColumnMissing(kValueCol) && !_indexedSequences.contains(_lastSeq) )
                _items.push_back({row, nullptr, ItemStatus::Unset});
        }
        _incomplete = (row == limit);
        if ( !_incomplete ) _lastSeq = _atSeq;
        _count = _items.size();
    }

    FLValue LazyIndexUpdate::valueAt(size_t i) const {
        AssertArg(i < _count);
        _enum->seek(_items[i].queryRow);
        return FLValue(_enum->columns()[kValueCol]);
    }

    void LazyIndexUpdate::setVectorAt(size_t i, const float* vec, size_t dimension) {
        AssertArg(i < _count);
        unique_ptr<float[]> heapVec;
        if ( vec != nullptr ) {
            AssertArg(dimension > 0);
            if ( _dimension == 0 ) {
                _dimension = dimension;
            } else if ( dimension != _dimension ) {
                error::_throw(error::InvalidParameter, "Inconsistent vector dimensions");
            }
            heapVec = unique_ptr<float[]>(new float[dimension]);
            std::copy(vec, vec + dimension, heapVec.get());
        }
        _items[i].vector = std::move(heapVec);
        _items[i].status = ItemStatus::Set;
    }

    void LazyIndexUpdate::skipVectorAt(size_t i) {
        AssertArg(i < _count);
        _items[i].vector = nullptr;
        _items[i].status = ItemStatus::Skipped;
    }

    bool LazyIndexUpdate::finish(ExclusiveTransaction& txn) {
        // Finishing an update without either updating or skipping at least one vector is unsupported.
        if ( anyVectorNotModified() ) {
            litecore::error::_throw(litecore::error::UnsupportedOperation,
                                    "Cannot finish an update without all vectors updated or skipped.");
        }

        sequence_t curSeq = _manager->_sqlKeyStore.lastSequence();

        // First mark all sequences covered by the query as indexed:
        SequenceSet newIndexedSequences = _indexedSequences;
        newIndexedSequences.add(_firstSeq, _lastSeq + 1);

        std::set<int64_t> obsoleteRowids;
        if ( curSeq > _atSeq ) {
            // There have been more changes since the update began. Find docs that changed:
            auto&          stmt = _manager->_sqlKeyStore.compileCached("SELECT rowid FROM kv_@ WHERE sequence > ?1");
            UsingStatement u(stmt);
            stmt.bind(1, (long long)(_atSeq));
            while ( stmt.executeStep() ) obsoleteRowids.insert(stmt.getColumn(0).getInt64());
        }

        SequenceSet skippedSequences;
        auto        item = _items.begin();
        _enum->seek(-1);
        for ( size_t row = 0; _enum->next(); ++row ) {
            sequence_t seq{_enum->columns()[kSequenceCol]->asUnsigned()};
            if ( !_indexedSequences.contains(seq) ) {
                VectorPtr vec;
                bool      skip = false;
                if ( !_enum->isColumnMissing(kValueCol) ) {
                    Assert(item->queryRow == row);
                    vec  = std::move(item->vector);
                    skip = item->status == ItemStatus::Skipped;
                    ++item;
                }
                int64_t rowid = _enum->columns()[kRowIDCol]->asInt();
                if ( obsoleteRowids.find(rowid) == obsoleteRowids.end() ) {
                    if ( vec ) _manager->insert(rowid, vec.get(), _dimension);
                    else if ( skip )
                        newIndexedSequences.remove(seq);  // Mark skipped sequence as not indexed
                    else
                        _manager->del(rowid);
                }
            }
        }
        Assert(item == _items.end());

        _enum  = nullptr;
        _count = 0;
        _items.clear();

        _manager->updateIndexedSequences(newIndexedSequences);
        _manager = nullptr;

        return newIndexedSequences.contains(sequence_t{1}, curSeq + 1);
    }

    /// Returns true if any vector has NOT been updated or skipped in this updater.
    bool LazyIndexUpdate::anyVectorNotModified() const {
        return std::any_of(_items.begin(), _items.end(),
                           [](const Item& item) { return item.status == ItemStatus::Unset; });
    }


}  // namespace litecore

#endif
