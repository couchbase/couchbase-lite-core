//
//  c4QueryImpl.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/20.
//  Copyright 2020-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#pragma once
#include "c4Query.hh"
#include "c4Internal.hh"

#include "DatabaseImpl.hh"
#include "Query.hh"
#include "Array.hh"
#include <mutex>
#include <utility>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {
    using namespace fleece::impl;

    // Encapsulates C4QueryEnumerator struct. A C4QueryEnumerator* points inside this object.
    class C4QueryEnumeratorImpl
        : public RefCounted
        , public C4QueryEnumerator
        , public fleece::InstanceCountedIn<C4QueryEnumerator>
        , C4Base {
      public:
        C4QueryEnumeratorImpl(DatabaseImpl* database, Query* query, QueryEnumerator* e)
            : _database(database), _query(query), _enum(e), _hasFullText(_enum->hasFullText()) {
            clearPublicFields();
        }

        QueryEnumerator* enumerator() const {
            if ( !_enum ) error::_throw(error::InvalidParameter, "Query enumerator has been closed");
            return _enum;
        }

        int64_t getRowCount() const { return enumerator()->getRowCount(); }

        bool next() {
            if ( !enumerator()->next() ) {
                clearPublicFields();
                return false;
            }
            populatePublicFields();
            return true;
        }

        void seek(int64_t rowIndex) {
            enumerator()->seek(rowIndex);
            if ( rowIndex >= 0 ) populatePublicFields();
            else
                clearPublicFields();
        }

        void clearPublicFields() { ::memset((C4QueryEnumerator*)this, 0, sizeof(C4QueryEnumerator)); }

        void populatePublicFields() {
            static_assert(sizeof(C4FullTextMatch) == sizeof(Query::FullTextTerm),
                          "C4FullTextMatch does not match Query::FullTextTerm");
            (Array::iterator&)columns = _enum->columns();
            missingColumns            = _enum->missingColumns();
            if ( _hasFullText ) {
                auto& ft           = _enum->fullTextTerms();
                fullTextMatches    = (const C4FullTextMatch*)ft.data();
                fullTextMatchCount = (uint32_t)ft.size();
            }
        }

        C4QueryEnumeratorImpl* C4NULLABLE refresh() {
            QueryEnumerator* newEnum = enumerator()->refresh(_query);
            if ( newEnum ) return retain(new C4QueryEnumeratorImpl(_database, _query, newEnum));
            else
                return nullptr;
        }

        void close() noexcept { _enum = nullptr; }

        bool usesEnumerator(QueryEnumerator* e) const { return e == _enum; }

      private:
        Retained<DatabaseImpl>    _database;
        Retained<Query>           _query;
        Retained<QueryEnumerator> _enum;
        bool                      _hasFullText;
    };

    static inline C4QueryEnumeratorImpl* asInternal(C4QueryEnumerator* e) { return (C4QueryEnumeratorImpl*)e; }

    // Internal implementation of C4QueryObserver
    class C4QueryObserverImpl : public C4QueryObserver {
      public:
        C4QueryObserverImpl(C4Query* query, C4Query::ObserverCallback callback)
            : C4QueryObserver(query), _callback(move(callback)) {}

        ~C4QueryObserverImpl() {
            if ( _query ) _query->enableObserver(this, false);
        }

        void setEnabled(bool enabled) override { _query->enableObserver(this, enabled); }

        // called on a background thread
        void notify(C4QueryEnumeratorImpl* e, C4Error err) noexcept {
            {
                LOCK(_mutex);
                _currentEnumerator = e;
                _currentError      = err;
            }
            _callback(this);
        }

        Retained<C4QueryEnumeratorImpl> getEnumeratorImpl(bool forget, C4Error* C4NULLABLE outError) {
            LOCK(_mutex);
            if ( outError ) *outError = _currentError;
            if ( forget ) return std::move(_currentEnumerator);
            else
                return _currentEnumerator;
        }

        C4Query::Enumerator getEnumerator(bool forget) override {
            if ( _currentError.code ) _currentError.raise();
            Retained<QueryEnumerator> e = _currentEnumerator->enumerator();
            if ( forget ) _currentEnumerator = nullptr;
            return C4Query::Enumerator(move(e));
        }

      private:
        C4Query::ObserverCallback const _callback;
        mutable std::mutex              _mutex;
        Retained<C4QueryEnumeratorImpl> _currentEnumerator;
    };

    static inline C4QueryObserverImpl* asInternal(C4QueryObserver* obs) { return (C4QueryObserverImpl*)obs; }

}  // namespace litecore

C4_ASSUME_NONNULL_END
