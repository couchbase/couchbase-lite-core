//
//  c4QueryEnumeratorImpl.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/20.
//  Copyright © 2020 Couchbase. All rights reserved.
//

#pragma once
#include "c4Internal.hh"
#include "c4Query.h"
#include "c4Database.hh"

#include "Query.hh"
#include "InstanceCounted.hh"
#include "RefCounted.hh"
#include "Array.hh"

using namespace litecore;
using namespace fleece::impl;
namespace c4Internal {

    // Encapsulates C4QueryEnumerator struct. A C4QueryEnumerator* points inside this object.
    struct C4QueryEnumeratorImpl : public RefCounted,
                                   public C4QueryEnumerator,
                                   fleece::InstanceCountedIn<C4QueryEnumerator>
    {
        C4QueryEnumeratorImpl(Database *database, Query *query, QueryEnumerator *e)
        :_database(database)
        ,_query(query)
        ,_enum(e)
        ,_hasFullText(_enum->hasFullText())
        {
            clearPublicFields();
        }

        QueryEnumerator& enumerator() const {
            if (!_enum)
                error::_throw(error::InvalidParameter, "Query enumerator has been closed");
            return *_enum;
        }

        int64_t getRowCount() const {
            return enumerator().getRowCount();
        }

        bool next() {
            if (!enumerator().next()) {
                clearPublicFields();
                return false;
            }
            populatePublicFields();
            return true;
        }

        void seek(int64_t rowIndex) {
            enumerator().seek(rowIndex);
            if (rowIndex >= 0)
                populatePublicFields();
            else
                clearPublicFields();
        }

        void clearPublicFields() {
            ::memset((C4QueryEnumerator*)this, 0, sizeof(C4QueryEnumerator));
        }

        void populatePublicFields() {
            static_assert(sizeof(C4FullTextMatch) == sizeof(Query::FullTextTerm),
                          "C4FullTextMatch does not match Query::FullTextTerm");
            (Array::iterator&)columns = _enum->columns();
            missingColumns = _enum->missingColumns();
            if (_hasFullText) {
                auto &ft = _enum->fullTextTerms();
                fullTextMatches = (const C4FullTextMatch*)ft.data();
                fullTextMatchCount = (uint32_t)ft.size();
            }
        }

        C4QueryEnumeratorImpl* refresh() {
            QueryEnumerator* newEnum = enumerator().refresh(_query);
            if (newEnum)
                return retain(new C4QueryEnumeratorImpl(_database, _query, newEnum));
            else
                return nullptr;
        }

        void close() noexcept {
            _enum = nullptr;
        }

        bool usesEnumerator(QueryEnumerator *e) const {
            return e == _enum;
        }

    private:
        Retained<Database> _database;
        Retained<Query> _query;
        Retained<QueryEnumerator> _enum;
        bool _hasFullText;
    };

    
    static inline C4QueryEnumeratorImpl* asInternal(C4QueryEnumerator *e) {
        return (C4QueryEnumeratorImpl*)e;
    }

}
