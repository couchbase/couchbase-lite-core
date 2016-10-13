//
//  Query.hh
//  LiteCore
//
//  Created by Jens Alfke on 10/5/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "KeyStore.hh"

namespace litecore {
    class Query;


    class QueryEnumerator {
    public:
        struct Options {
            Options()           :skip(0), limit(UINT64_MAX) { }
            uint64_t skip;
            uint64_t limit;
            slice paramBindings;
        };

        QueryEnumerator(Query*, const Options* =nullptr);

        bool next();
        void close()                    {_impl.reset();}

        slice recordID() const             {return _recordID;}
        sequence_t sequence() const     {return _sequence;}

        class Impl {
        public:
            virtual ~Impl() = default;
            virtual bool next(slice &docID, sequence_t &sequence) =0;
        };

    private:
        std::unique_ptr<Impl> _impl;
        slice _recordID;
        sequence_t _sequence;
    };


    /** Abstract base class of compiled database queries.
        These are created by the factory method KeyStore::compileQuery(). */
    class Query {
    public:
        virtual ~Query() = default;

        KeyStore& keyStore() const      {return _keyStore;}

    protected:
        Query(KeyStore &keyStore) noexcept
        :_keyStore(keyStore)
        { }

        virtual QueryEnumerator::Impl* createEnumerator(const QueryEnumerator::Options*) =0;

    private:
        KeyStore &_keyStore;

        friend class QueryEnumerator;
    };

}
