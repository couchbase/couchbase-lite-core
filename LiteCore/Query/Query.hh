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
        KeyStore& keyStore() const                                      {return _keyStore;}

        virtual unsigned columnCount() const noexcept                   {return 0;}
        virtual std::string nameOfColumn(unsigned col) const            {return "";}

        virtual alloc_slice getMatchedText(slice recordID, sequence_t)  {return alloc_slice();}

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

        slice recordID() const                                  {return _recordID;}
        sequence_t sequence() const                             {return _sequence;}
        slice version() const                                   {return _version;}
        DocumentFlags flags() const                             {return _flags;}

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
        virtual alloc_slice getMatchedText() const              {return alloc_slice();}

        /** If the query results have changed since `currentEnumerator`, returns a new enumerator
            that will return the new results. Otherwise returns null. */
        virtual QueryEnumerator* refresh() =0;

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
