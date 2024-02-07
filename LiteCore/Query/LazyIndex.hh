//
// LazyIndex.hh
//
// Copyright © 2024 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"
#include "fleece/Fleece.h"
#include <vector>

namespace SQLite {
    class Statement;
}

namespace litecore {
    class ExclusiveTransaction;
    class KeyStore;
    class LazyIndexUpdate;
    class Query;
    class QueryEnumerator;
    class SQLiteDataFile;
    struct SQLiteIndexSpec;
    class SQLiteKeyStore;

    /** Represents a lazy index. Acts as a factory for LazyIndexUpdate objects. */
    class LazyIndex : public fleece::RefCounted {
      public:
        LazyIndex(KeyStore& keyStore, string_view indexName);

        KeyStore& keyStore() { return _keyStore; }

        string_view indexName() const { return _indexName; }

        /// Creates a LazyIndexUpdate representing the vectors that need to be recomputed to bring
        /// the index up to date; or just returns nullptr if the index is already up-to-date.
        Retained<LazyIndexUpdate> beginUpdate(size_t limit);

      private:
        friend class LazyIndexUpdate;

        SQLiteIndexSpec getSpec() const;
        void            insert(int64_t rowid, float vec[], size_t dimension);
        void            del(int64_t rowid);
        void            updateLastSequence(sequence_t);

        KeyStore&                          _keyStore;         // The public KeyStore
        string                             _indexName;        // The index's name
        SQLiteDataFile&                    _db;               // The index's DataFile
        SQLiteKeyStore&                    _sqlKeyStore;      // The actual SQLiteKeyStore
        string                             _vectorTableName;  // The index's table's name
        Retained<Query>                    _query;            // Query that finds updated docs
        std::unique_ptr<SQLite::Statement> _ins, _del;        // Statements to update the index
    };

    /** Describes a set of index values that need to be computed by the application,
        to update a lazy index after its KeyStore has changed.
        You should:
        1. Call `valueAt` for each of the `count` items to get the Fleece value, and:
          1.1. Compute a vector from this value
          1.2. Call `setVectorAt` with the resulting vector, or with nullptr if none.
        2. Finally, open a transaction and call `finish` to apply the updates to the index. */
    class LazyIndexUpdate : public fleece::RefCounted {
      public:
        /// The number of vectors to compute.
        size_t count() const { return _count; }

        /// Returns the i'th value to compute a vector from.
        /// This is the value of the expression in the index spec.
        FLValue valueAt(size_t i) const;

        /// Sets the vector for the i'th value. If you don't call this, it's assumed there is no
        /// vector, and any existing vector will be removed upon `finish`.
        void setVectorAt(size_t i, const float* vector, size_t dimension);

        /// Updates the index with the computed vectors, removes any index rows for which no vector
        /// was given, and updates the index's latest sequence.
        /// @returns  True if the index is now completely up-to-date; false if there have been
        ///           changes to the KeyStore since the LazyIndexUpdate was created.
        bool finish(ExclusiveTransaction&);

      private:
        friend class LazyIndex;
        LazyIndexUpdate(LazyIndex*, sequence_t curSeq, Retained<QueryEnumerator>, size_t limit);

        using VectorPtr = std::unique_ptr<float[]>;

        Retained<LazyIndex>       _manager;        // Owning LazyIndex
        sequence_t                _atSeq;          // KeyStore's lastSequence at time of query
        Retained<QueryEnumerator> _enum;           // Results of Query for updated docs
        size_t                    _count = 0;      // Number of vectors to update
        std::vector<size_t>       _rows;           // Maps public value# to QueryEnum row#
        std::vector<VectorPtr>    _vectors;        // The vectors computed by the app
        size_t                    _dimension = 0;  // Dimensions of the vectors in _vectors
        bool                      _incomplete;     // True if query did not get all update docs
    };


}  // namespace litecore
