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
#include "Error.hh"
#include "Logging.hh"
#include <atomic>
#include <vector>

namespace fleece::impl {
    class ArrayIterator;
}

namespace litecore {
    class QueryEnumerator;


    /** Abstract base class of compiled database queries.
        These are created by the factory method KeyStore::compileQuery(). */
    class Query : public RefCounted, public Logging {
    public:

        class parseError : public error {
        public:
            parseError(const char *message, int errPos);

            const int errorPosition;
        };

        /** Info about a match of a full-text query term */
        struct FullTextTerm {
            uint64_t dataSource;              ///< Opaque identifier of where text is stored
            uint32_t keyIndex;                ///< Which index key the match occurred in
            uint32_t termIndex;               ///< Index of the search term in the tokenized query
            uint32_t start, length;           ///< *Byte* range of word in query string
        };


        KeyStore& keyStore() const;
        alloc_slice expression() const                                  {return _expression;}
        QueryLanguage language() const                                  {return _language;}

        virtual unsigned columnCount() const noexcept =0;
        
        virtual const std::vector<std::string>& columnTitles() const noexcept =0;

        virtual alloc_slice getMatchedText(const FullTextTerm&) =0;

        virtual std::string explain() =0;

        virtual void close()                                            {_keyStore = nullptr;}

        struct Options {
            Options() { }
            
            Options(const Options &o)
            :paramBindings(o.paramBindings), afterSequence(o.afterSequence) { }

            template <class T>
            Options(T bindings, sequence_t afterSeq =0, uint64_t withPurgeCount =0)
            :paramBindings(bindings), afterSequence(afterSeq), purgeCount(withPurgeCount) { }

            Options after(sequence_t afterSeq) const {return Options(paramBindings, afterSeq, purgeCount);}
            Options withPurgeCount(uint64_t purgeCnt) const {return Options(paramBindings, afterSequence, purgeCnt);}

            bool notOlderThan(sequence_t afterSeq, uint64_t purgeCnt) const {
                return afterSequence > 0 && afterSequence >= afterSeq && purgeCnt == purgeCount;
            }

            alloc_slice const paramBindings;
            sequence_t const  afterSequence {0};
            uint64_t const purgeCount {0};
        };

        virtual QueryEnumerator* createEnumerator(const Options* =nullptr) =0;

    protected:
        Query(KeyStore &keyStore, slice expression, QueryLanguage language);
        virtual ~Query();
        virtual std::string loggingIdentifier() const override;
        
    private:
        KeyStore* _keyStore;
        alloc_slice _expression;
        QueryLanguage _language;
    };


    /** Iterator/enumerator of query results. Abstract class created by Query::createEnumerator. */
    class QueryEnumerator : public RefCounted {
    public:
        using FullTextTerms = std::vector<Query::FullTextTerm>;

        const Query::Options& options() const                   {return _options;}
        sequence_t lastSequence() const                         {return _lastSequence;}
        uint64_t purgeCount() const                             {return _purgeCount;}

        virtual bool next() =0;

        virtual fleece::impl::ArrayIterator columns() const noexcept =0;
        virtual uint64_t missingColumns() const noexcept =0;
        
        /** Random access to rows. May not be supported by all implementations, but does work with
            the current SQLite query implementation. */
        virtual int64_t getRowCount() const         {return -1;}
        virtual void seek(int64_t rowIndex)         {error::_throw(error::UnsupportedOperation);}

        virtual bool hasFullText() const                        {return false;}
        virtual const FullTextTerms& fullTextTerms()            {return _fullTextTerms;}

        /** If the query results have changed since I was created, returns a new enumerator
            that will return the new results. Otherwise returns null. */
        virtual QueryEnumerator* refresh(Query *query) =0;

        virtual bool obsoletedBy(const QueryEnumerator*) =0;

    protected:
        QueryEnumerator(const Query::Options *options, sequence_t lastSeq, uint64_t purgeCount)
        :_options(options ? *options : Query::Options{})
        ,_lastSequence(lastSeq)
        ,_purgeCount(purgeCount)
        { }
        virtual ~QueryEnumerator() =default;

        Query::Options _options;
        std::atomic<sequence_t> _lastSequence;       // DB's lastSequence at the time the query ran
        std::atomic<uint64_t> _purgeCount;           // DB's purgeCount at the time the query ran
        // The implementation of fullTextTerms() should populate this and return a reference:
        FullTextTerms _fullTextTerms;
    };

}
