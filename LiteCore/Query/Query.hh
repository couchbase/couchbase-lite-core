//
// Query.hh
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
#include "RefCounted.hh"
#include "KeyStore.hh"
#include "Fleece.hh"
#include "Error.hh"

namespace litecore {
    class QueryEnumerator;


    /** Abstract base class of compiled database queries.
        These are created by the factory method KeyStore::compileQuery(). */
    class Query : public RefCounted {
    public:

        /** Info about a match of a full-text query term */
        struct FullTextTerm {
            uint64_t dataSource;              ///< Opaque identifier of where text is stored
            uint32_t keyIndex;                ///< Which index key the match occurred in
            uint32_t termIndex;               ///< Index of the search term in the tokenized query
            uint32_t start, length;           ///< *Byte* range of word in query string
        };


        KeyStore& keyStore() const                                      {return _keyStore;}

        virtual unsigned columnCount() const noexcept =0;

        virtual alloc_slice getMatchedText(const FullTextTerm&) =0;

        virtual std::string explain() =0;

        struct Options {
            alloc_slice paramBindings;
        };

        virtual QueryEnumerator* createEnumerator(const Options* =nullptr) =0;

    protected:
        Query(KeyStore &keyStore) noexcept
        :_keyStore(keyStore)
        { }
        
        virtual ~Query() =default;

    private:
        KeyStore &_keyStore;
    };


    /** Iterator/enumerator of query results. Abstract class created by Query::createEnumerator. */
    class QueryEnumerator {
    public:
        using FullTextTerms = std::vector<Query::FullTextTerm>;

        virtual ~QueryEnumerator() =default;

        virtual bool next() =0;

        virtual fleece::Array::iterator columns() const noexcept =0;
        virtual uint64_t missingColumns() const noexcept =0;
        
        /** Random access to rows. May not be supported by all implementations, but does work with
            the current SQLite query implementation. */
        virtual int64_t getRowCount() const         {return -1;}
        virtual void seek(uint64_t rowIndex)        {error::_throw(error::UnsupportedOperation);}

        virtual bool hasFullText() const                        {return false;}
        virtual const FullTextTerms& fullTextTerms()            {return _fullTextTerms;}

        /** If the query results have changed since `currentEnumerator`, returns a new enumerator
            that will return the new results. Otherwise returns null. */
        virtual QueryEnumerator* refresh() =0;

    protected:
        // The implementation of fullTextTerms() should populate this and return a reference:
        FullTextTerms _fullTextTerms;
    };

}
