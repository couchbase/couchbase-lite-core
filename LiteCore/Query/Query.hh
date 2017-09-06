//
//  Query.hh
//  LiteCore
//
//  Created by Jens Alfke on 10/5/16.
//  Copyright © 2016 Couchbase. All rights reserved.
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
        using FullTextID = uint64_t;

        KeyStore& keyStore() const                                      {return _keyStore;}

        virtual unsigned columnCount() const noexcept                   {return 0;}
        virtual std::string nameOfColumn(unsigned col) const            {return "";}

        virtual alloc_slice getMatchedText(FullTextID)                  {return alloc_slice();}

        virtual std::string explain()                                   {return "";}

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
        virtual ~QueryEnumerator() =default;

        virtual bool next() =0;

        virtual fleece::Array::iterator columns() const noexcept =0;

        /** Random access to rows. May not be supported by all implementations, but does work with
            the current SQLite query implementation. */
        virtual int64_t getRowCount() const         {return -1;}
        virtual void seek(uint64_t rowIndex)        {error::_throw(error::UnsupportedOperation);}

        /** Info about a match of a full-text query term */
        struct FullTextTerm {
            uint32_t termIndex;               ///< Index of the search term in the tokenized query
            uint32_t start, length;           ///< *Byte* range of word in query string
        };

        virtual bool hasFullText() const                        {return false;}
        virtual const std::vector<FullTextTerm>& fullTextTerms(){return _fullTextTerms;}
        virtual Query::FullTextID fullTextID() const            {return 0;}

        /** If the query results have changed since `currentEnumerator`, returns a new enumerator
            that will return the new results. Otherwise returns null. */
        virtual QueryEnumerator* refresh() =0;

    protected:
        // The implementation of fullTextTerms() should populate this and return a reference:
        std::vector<FullTextTerm> _fullTextTerms;
    };

}
