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
struct C4Query final : public fleece::RefCounted,
                       public fleece::InstanceCountedIn<C4Query>,
                       C4Base
{
public:
    /// Creates a new query on a database.
    static Retained<C4Query> newQuery(C4Database*,
                                      C4QueryLanguage,
                                      slice queryExpression,
                                      int* C4NULLABLE outErrorPos);

    /// Creates a new query on the collection's database.
    /// If the query does not refer to a collection by name (e.g. "FROM airlines"),
    /// it will use the given collection instead of the default one.
    static Retained<C4Query> newQuery(C4Collection*,
                                      C4QueryLanguage,
                                      slice queryExpression,
                                      int* C4NULLABLE outErrorPos);

    unsigned columnCount() const noexcept;
    slice columnTitle(unsigned col) const;
    alloc_slice explain() const;

    alloc_slice parameters() const noexcept;
    void setParameters(slice parameters);

    alloc_slice fullTextMatched(const C4FullTextMatch&);

    // Running the query:

    /// C++ query enumerator; equivalent to C4QueryEnumerator but more C++-friendly.
    class Enumerator {
    public:
        bool next();
        int64_t rowCount() const;
        void seek(int64_t rowIndex);

        FLArrayIterator columns() const;
        FLValue column(unsigned i) const;

        unsigned fullTextMatchCount() const;
        C4FullTextMatch fullTextMatch(unsigned i) const;

        bool restart();
        void close() noexcept;

        Enumerator(Enumerator&&);
        ~Enumerator();

    private:
        friend struct C4Query;
        friend class litecore::C4QueryObserverImpl;
        explicit Enumerator(C4Query*, const C4QueryOptions* C4NULLABLE =nullptr,
                            slice encodedParameters =fleece::nullslice);
        explicit Enumerator(Retained<litecore::QueryEnumerator> e);
        
        Retained<litecore::QueryEnumerator> _enum;
        Retained<litecore::Query> _query;
    };

    /// Runs the query, returning an enumerator. Use it like this:
    /// ```
    /// auto e = query.run();
    /// while (e.next()) { ... }
    /// ```
    Enumerator run(const C4QueryOptions* C4NULLABLE opt =nullptr,
                   slice params =fleece::nullslice);

    /// Creates a C-style enumerator. Prefer \ref run to this.
    C4QueryEnumerator* createEnumerator(const C4QueryOptions* C4NULLABLE,
                                        slice params =fleece::nullslice);

    // Observer:

    using ObserverCallback = std::function<void(C4QueryObserver*)>;

    std::unique_ptr<C4QueryObserver> observe(ObserverCallback);

protected:
    friend class litecore::C4QueryObserverImpl;

    C4Query(C4Collection*, C4QueryLanguage language, slice queryExpression);
    ~C4Query();
    void enableObserver(litecore::C4QueryObserverImpl *obs, bool enable);

private:
    class LiveQuerierDelegate;
    
    Retained<litecore::QueryEnumerator> _createEnumerator(const C4QueryOptions* C4NULLABLE, slice params);
    Retained<litecore::C4QueryEnumeratorImpl> wrapEnumerator(litecore::QueryEnumerator* C4NULLABLE);
    void liveQuerierUpdated(litecore::QueryEnumerator* C4NULLABLE, C4Error err);
    void notifyObservers(const std::set<litecore::C4QueryObserverImpl*> &observers,
                         litecore::QueryEnumerator* C4NULLABLE, C4Error err);

    Retained<litecore::DatabaseImpl>            _database;
    Retained<litecore::Query>                   _query;
    alloc_slice                                 _parameters;
    Retained<litecore::LiveQuerier>             _bgQuerier;
    std::unique_ptr<LiveQuerierDelegate>        _bgQuerierDelegate;
    std::set<litecore::C4QueryObserverImpl*>    _observers;
    std::set<litecore::C4QueryObserverImpl*>    _pendingObservers;
    mutable std::mutex                          _mutex;
};



/** A registration for callbacks whenever a query's result set changes.
    The registration lasts until this object is destructed.
    Created by calling \ref C4Query::observe. */
struct C4QueryObserver : public fleece::InstanceCounted, C4Base {
public:
    virtual ~C4QueryObserver() = default;

    C4Query* query() const                  {return _query;}

    virtual void setEnabled(bool enabled) =0;

    /// If the latest run of the query failed, the error will be stored here, with nonzero `code`.
    /// Always check the error before getting the enumerator.
    C4Error getError() const                {return _currentError;}

    /// Returns a new enumerator on the query results.
    /// If the query failed, throws that error as an exception.
    virtual C4Query::Enumerator getEnumerator(bool forget =true) =0;

protected:
    C4QueryObserver(C4Query *query) :_query(query) { }
    
    Retained<C4Query>                           _query;
    C4Error                                     _currentError {};
};


C4_ASSUME_NONNULL_END
