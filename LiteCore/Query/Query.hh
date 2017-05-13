//
//  Query.hh
//  LiteCore
//
//  Created by Jens Alfke on 10/5/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "KeyStore.hh"
#include "Fleece.hh"

namespace litecore {
    class QueryEnumerator;


    /** Abstract base class of compiled database queries.
        These are created by the factory method KeyStore::compileQuery(). */
    class Query {
    public:
        virtual ~Query() = default;

        KeyStore& keyStore() const                                      {return _keyStore;}

        virtual unsigned columnCount() const noexcept                   {return 0;}
        virtual std::string nameOfColumn(unsigned col) const            {return "";}

        virtual alloc_slice getMatchedText(slice recordID, sequence_t)  {return alloc_slice();}

        virtual std::string explain()                                   {return "";}

        struct Options {
            uint64_t skip  {0};
            uint64_t limit {UINT64_MAX};
            slice paramBindings;
        };

        virtual QueryEnumerator* createEnumerator(const Options* =nullptr) =0;

    protected:
        Query(KeyStore &keyStore) noexcept
        :_keyStore(keyStore)
        { }

    private:
        KeyStore &_keyStore;
    };


    /** Iterator/enumerator of query results. Abstract class created by Query::createEnumerator. */
    class QueryEnumerator {
    public:
        virtual ~QueryEnumerator() =default;

        virtual bool next() =0;
        virtual void close() =0;

        slice recordID() const                                  {return _recordID;}
        sequence_t sequence() const                             {return _sequence;}
        slice version() const                                   {return _version;}
        DocumentFlags flags() const                             {return _flags;}

        virtual fleece::Array::iterator columns() const noexcept =0;

        /** Info about a match of a full-text query term */
        struct FullTextTerm {
            uint32_t termIndex;               ///< Index of the search term in the tokenized query
            uint32_t start, length;           ///< *Byte* range of word in query string
        };

        virtual bool hasFullText() const                        {return false;}
        virtual const std::vector<FullTextTerm>& fullTextTerms(){return _fullTextTerms;}
        virtual alloc_slice getMatchedText() const              {return alloc_slice();}

    protected:
        // The implementation of next() should set these:
        slice _recordID;
        sequence_t _sequence;
        slice _version;
        DocumentFlags _flags {DocumentFlags::kNone};
        // The implementation of fullTextTerms() should populate this and return a reference:
        std::vector<FullTextTerm> _fullTextTerms;
    };

}
