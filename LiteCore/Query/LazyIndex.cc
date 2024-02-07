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
#    include "QueryParser.hh"
#    include "SQLiteDataFile.hh"
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
                                "WHERE": [">", ["._sequence"], ["$lastSeq"]],
                                "ORDER_BY": [ ["._sequence"] ],
                                "LIMIT": ["$limit"]}])";
        _query = _db.compileQuery(queryJson, QueryLanguage::kJSON, &keyStore);
    }

    SQLiteIndexSpec LazyIndex::getSpec() const {
        optional<SQLiteIndexSpec> spec = _db.getIndex(_indexName);
        if ( !spec || spec->keyStoreName != _sqlKeyStore.name() )
            error::_throw(error::NoSuchIndex, "No such index in collection");
        if ( spec->lastSequence == nullopt ) error::_throw(error::UnsupportedOperation, "Index is not lazy");
        Assert(spec->type == IndexSpec::kVector);  // no other type supports laziness (yet)
        return *std::move(spec);
    }

    Retained<LazyIndexUpdate> LazyIndex::beginUpdate(size_t limit) {
        AssertArg(limit > 0);
        Retained<LazyIndexUpdate> update;
        do {
            {
                // Open a RO transaction so the code sees a consistent snapshot of the database:
                ReadOnlyTransaction txn(_db);

                sequence_t sinceSeq = getSpec().lastSequence.value();
                sequence_t curSeq   = _sqlKeyStore.lastSequence();
                if ( sinceSeq >= curSeq ) break;  // Index is up-to-date

                Encoder enc;
                enc.beginDict();
                enc["lastSeq"] = int64_t(sinceSeq);
                enc["limit"]   = limit;
                enc.endDict();
                Query::Options            options(enc.finish());
                Retained<QueryEnumerator> e = _query->createEnumerator(&options);
                if ( e->getRowCount() == 0 ) break;  // Index is up-to-date
                update = new LazyIndexUpdate(this, curSeq, e, limit);
            }

            if ( update && update->count() == 0 ) {
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
        _ins->bind(1, rowid);
        _ins->bindNoCopy(2, (const void*)vec, int(dimension * sizeof(float)));
        _ins->exec();
        _ins->reset();
    }

    void LazyIndex::del(int64_t rowid) {
        if ( !_del ) {
            _del = make_unique<SQLite::Statement>(
                    _db, CONCAT("DELETE FROM " << sqlIdentifier(_vectorTableName) << " WHERE docid=?1"));
        }
        _del->bind(1, rowid);
        _del->exec();
        _del->reset();
    }

    void LazyIndex::updateLastSequence(sequence_t seq) { _db.setIndexLastSequence(_indexName, seq); }

#    pragma mark - LAZY INDEX UPDATE:

    LazyIndexUpdate::LazyIndexUpdate(LazyIndex* manager, sequence_t atSeq, Retained<QueryEnumerator> e, size_t limit)
        : _manager(manager), _atSeq(atSeq), _enum(std::move(e)) {
        // Find the rows with a non-missing value:
        size_t row = 0;
        for ( ; _enum->next(); ++row ) {
            if ( (_enum->missingColumns() & (1 << kValueCol)) == 0 ) _rows.push_back(row);
        }
        _incomplete = (row == limit);
        _count      = _rows.size();
        _vectors.resize(_count);
    }

    FLValue LazyIndexUpdate::valueAt(size_t i) const {
        AssertArg(i < _count);
        _enum->seek(_rows[i]);
        return FLValue(_enum->columns()[kValueCol]);
    }

    void LazyIndexUpdate::setVectorAt(size_t i, const float* vec, size_t dimension) {
        AssertArg(i < _count);
        AssertArg(vec != nullptr);
        AssertArg(dimension > 0);
        if ( _dimension == 0 ) {
            _dimension = dimension;
        } else if ( dimension != _dimension ) {
            error::_throw(error::InvalidParameter, "Inconsistent vector dimensions");
        }
        auto heapVec = unique_ptr<float[]>(new float[dimension]);
        std::copy(vec, vec + dimension, heapVec.get());
        _vectors[i] = std::move(heapVec);
    }

    bool LazyIndexUpdate::finish(ExclusiveTransaction& txn) {
        sequence_t        curSeq = _manager->_sqlKeyStore.lastSequence();
        sequence_t        newSeq = _atSeq;
        std::set<int64_t> obsolete;
        if ( curSeq > _atSeq ) {
            // There have been more changes since the update began. Find docs that changed:
            auto&          stmt = _manager->_sqlKeyStore.compileCached("SELECT rowid FROM kv_@ WHERE sequence > ?1");
            UsingStatement u(stmt);
            stmt.bind(1, int64_t(_atSeq));
            while ( stmt.executeStep() ) obsolete.insert(stmt.getColumn(0).getInt64());
        }

        size_t i = 0;
        _enum->seek(-1);
        for ( size_t row = 0; _enum->next(); ++row ) {
            VectorPtr vec;
            if ( (_enum->missingColumns() & (1 << kValueCol)) == 0 ) vec = std::move(_vectors[i++]);
            int64_t rowid = _enum->columns()[kRowIDCol]->asInt();
            if ( obsolete.find(rowid) == obsolete.end() ) {
                if ( vec ) _manager->insert(rowid, vec.get(), _dimension);
                else
                    _manager->del(rowid);
            }
            if ( _incomplete ) newSeq = sequence_t(_enum->columns()[kSequenceCol]->asInt());
        }
        assert(i == _count);

        _enum  = nullptr;
        _count = 0;
        _rows.clear();
        _vectors.clear();

        _manager->updateLastSequence(newSeq);
        _manager = nullptr;

        return newSeq == curSeq;
    }


}  // namespace litecore

#endif
