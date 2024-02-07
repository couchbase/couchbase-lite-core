//
// c4Query.hh
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.hh"
#include "c4QueryTypes.h"
#include "fleece/InstanceCounted.hh"
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

C4_ASSUME_NONNULL_BEGIN

// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


/** A compiled database query. */
struct C4Query final
    : public fleece::RefCounted
    , public fleece::InstanceCountedIn<C4Query>
    , C4Base {
  public:
    /// Creates a new query on a database.
    static Retained<C4Query> newQuery(C4Database*, C4QueryLanguage, slice queryExpression, int* C4NULLABLE outErrorPos);

    /// Creates a new query on the collection's database.
    /// If the query does not refer to a collection by name (e.g. "FROM airlines"),
    /// it will use the given collection instead of the default one.
    static Retained<C4Query> newQuery(C4Collection*, C4QueryLanguage, slice queryExpression,
                                      int* C4NULLABLE outErrorPos);

    unsigned    columnCount() const noexcept;
    slice       columnTitle(unsigned col) const;
    alloc_slice explain() const;

    alloc_slice parameters() const noexcept;
    void        setParameters(slice parameters);

    alloc_slice fullTextMatched(const C4FullTextMatch&);

    // Running the query:

    /// C++ query enumerator; equivalent to C4QueryEnumerator but more C++-friendly.
    class Enumerator {
      public:
        bool                  next();
        [[nodiscard]] int64_t rowCount() const;
        void                  seek(int64_t rowIndex);

        [[nodiscard]] FLArrayIterator columns() const;
        [[nodiscard]] FLValue         column(unsigned i) const;

        [[nodiscard]] unsigned        fullTextMatchCount() const;
        [[nodiscard]] C4FullTextMatch fullTextMatch(unsigned i) const;

        bool restart();
        void close() noexcept;

        Enumerator(Enumerator&&) noexcept;
        ~Enumerator();

      private:
        friend struct C4Query;
        friend class litecore::C4QueryObserverImpl;
        explicit Enumerator(C4Query*, slice encodedParameters = fleece::nullslice);
        explicit Enumerator(Retained<litecore::QueryEnumerator> e);

        Retained<litecore::QueryEnumerator> _enum;
        Retained<litecore::Query>           _query;
    };

    /// Runs the query, returning an enumerator. Use it like this:
    /// ```
    /// auto e = query.run();
    /// while (e.next()) { ... }
    /// ```
    Enumerator run(slice params = fleece::nullslice);

    /// Creates a C-style enumerator. Prefer \ref run to this.
    C4QueryEnumerator* createEnumerator(slice params = fleece::nullslice);

    // Observer:

    using ObserverCallback = std::function<void(C4QueryObserver*)>;

    std::unique_ptr<C4QueryObserver> observe(const ObserverCallback&);

  protected:
    friend class litecore::C4QueryObserverImpl;

    C4Query(C4Collection*, C4QueryLanguage language, slice queryExpression);
    ~C4Query() override;
    void enableObserver(litecore::C4QueryObserverImpl* obs, bool enable);

  private:
    class LiveQuerierDelegate;

    Retained<litecore::QueryEnumerator>       _createEnumerator(slice params);
    Retained<litecore::C4QueryEnumeratorImpl> wrapEnumerator(litecore::QueryEnumerator* C4NULLABLE);
    void                                      liveQuerierUpdated(litecore::QueryEnumerator* C4NULLABLE, C4Error err);
    void                                      liveQuerierStopped();
    void                                      notifyObservers(const std::set<litecore::C4QueryObserverImpl*>& observers,
                                                              litecore::QueryEnumerator* C4NULLABLE, C4Error err);

    Retained<litecore::DatabaseImpl>         _database;
    Retained<litecore::Query>                _query;
    alloc_slice                              _parameters;
    Retained<litecore::LiveQuerier>          _bgQuerier;
    std::unique_ptr<LiveQuerierDelegate>     _bgQuerierDelegate;
    std::set<litecore::C4QueryObserverImpl*> _observers;
    std::set<litecore::C4QueryObserverImpl*> _pendingObservers;
    mutable std::mutex                       _mutex;
};

/** A registration for callbacks whenever a query's result set changes.
    The registration lasts until this object is destructed.
    Created by calling \ref C4Query::observe. */
struct C4QueryObserver
    : public fleece::InstanceCounted
    , C4Base {
  public:
    ~C4QueryObserver() override = default;

    [[nodiscard]] C4Query* query() const { return _query; }

    virtual void setEnabled(bool enabled) = 0;

    /// If the latest run of the query failed, the error will be stored here, with nonzero `code`.
    /// Always check the error before getting the enumerator.
    [[nodiscard]] C4Error getError() const { return _currentError; }

    /// Returns a new enumerator on the query results.
    /// If the query failed, throws that error as an exception.
    virtual C4Query::Enumerator getEnumerator(bool forget = true) = 0;

  protected:
    explicit C4QueryObserver(C4Query* query) : _query(query) {}

    Retained<C4Query> _query;
    C4Error           _currentError{};
};

#ifdef COUCHBASE_ENTERPRISE

/** Represents a lazy index. Acts as a factory for C4LazyIndexUpdate objects. */
struct C4LazyIndex final
    : public fleece::RefCounted
    , public fleece::InstanceCountedIn<C4LazyIndex>
    , C4Base {
    /// Creates a C4LazyIndex object that can be used to update the index.
    static Retained<C4LazyIndex> open(C4Collection*, slice indexName);

    /// Finds new or updated documents for which vectors need to be recomputed by the application.
    /// If there are none, returns NULL. */
    Retained<struct C4LazyIndexUpdate> beginUpdate(size_t limit);

  private:
    C4LazyIndex(Retained<litecore::LazyIndex>, C4Collection*);
    ~C4LazyIndex();

    Retained<litecore::LazyIndex> _index;
    Retained<C4Collection>        _collection;
};

/** Describes a set of index values that need to be computed by the application,
    to update a lazy index after its Collection has changed.
    You should:
    1. Call `valueAt` for each of the `count` items to get the Fleece value, and:
      1.1. Compute a vector from this value
      1.2. Call `setVectorAt` with the resulting vector, or with nullptr if none.
    2. Finally, open a transaction and call `finish` to apply the updates to the index. */
struct C4LazyIndexUpdate final
    : public fleece::RefCounted
    , public fleece::InstanceCountedIn<C4LazyIndexUpdate>
    , C4Base {
    /// The number of vectors to compute.
    size_t count() const;

    /// Returns the i'th value to compute a vector from.
    /// This is the value of the expression in the index spec.
    FLValue valueAt(size_t i) const;

    /// Sets the vector for the i'th value. If you don't call this, it's assumed there is no
    /// vector, and any existing vector will be removed upon `finish`.
    void setVectorAt(size_t i, const float* vector, size_t dimension);

    /// Updates the index with the computed vectors, removes any index rows for which no vector
    /// was given, and updates the index's latest sequence.
    /// @note  Must be called in a database transaction.
    /// @returns  True if the index is now completely up-to-date; false if there have been
    ///           changes to the Collection since the LazyIndexUpdate was created.
    bool finish();

  private:
    friend struct C4LazyIndex;
    C4LazyIndexUpdate(Retained<litecore::LazyIndexUpdate>, C4Collection*);
    ~C4LazyIndexUpdate();

    Retained<litecore::LazyIndexUpdate> _update;
    Retained<C4Collection>              _collection;
};

#endif

C4_ASSUME_NONNULL_END
