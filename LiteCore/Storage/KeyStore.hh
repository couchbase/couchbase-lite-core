//
//  KeyStore.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 11/12/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"
#include "RecordEnumerator.hh"
#include "function_ref.hh"

namespace litecore {

    class DataFile;
    class Record;
    class Transaction;
    class Query;

    /** A sequence number in a KeyStore. */
    typedef uint64_t sequence_t;

    /** A container of key/value mappings. Keys and values are opaque blobs.
        The value is divided into 'meta' and 'body'; the body can optionally be omitted when
        reading, to save time/space. There is also a 'sequence' number that's assigned every time
        a value is saved, from an incrementing counter.
        A key, meta and body together are called a Record.
        This is an abstract class; the DataFile instance acts as its factory and will instantiate
        the appropriate subclass for the storage engine in use. */
    class KeyStore {
    public:

        struct Capabilities {
            bool sequences      :1;     ///< Records have sequences & can be enumerated by sequence

            static const Capabilities defaults;
        };


        DataFile& dataFile() const                  {return _db;}
        const std::string& name() const             {return _name;}
        Capabilities capabilities() const           {return _capabilities;}

        virtual uint64_t recordCount() const =0;
        virtual sequence_t lastSequence() const =0;

        virtual void erase() =0;

        void deleteKeyStore(Transaction&);

        // Keys/values:

        Record get(slice key, ContentOptions = kDefaultContent) const;
        virtual Record get(sequence_t, ContentOptions = kDefaultContent) const =0;

        virtual void get(slice key, ContentOptions, function_ref<void(const Record&)>);
        virtual void get(sequence_t, ContentOptions, function_ref<void(const Record&)>);

        /** Reads a record whose key() is already set. */
        virtual bool read(Record &rec, ContentOptions options = kDefaultContent) const =0;

        /** Reads the body of a Record that's already been read with kMetaonly.
            Does nothing if the record's body is non-null. */
        virtual void readBody(Record &rec) const;

        /** Creates a database query object. */
        virtual Query* compileQuery(slice expr);

        //////// Writing:

        virtual sequence_t set(slice key, slice vers, slice value, DocumentFlags, Transaction&) =0;

        sequence_t set(slice key, slice value, Transaction &t) {
            return set(key, nullslice, value, DocumentFlags::kNone, t);
        }

        void write(Record&, Transaction&);

        bool del(slice key, Transaction&);
        bool del(sequence_t s, Transaction&);
        bool del(const Record&, Transaction&);

        //////// INDEXING:

        enum IndexType {
            kValueIndex,         ///< Regular index of property value
            kFullTextIndex,      ///< Full-text index
            kGeoIndex,           ///< Geo index of GeoJSON values
        };

        struct IndexOptions {
            const char *stemmer;
            bool ignoreDiacritics;
        };

        virtual bool supportsIndexes(IndexType) const                   {return false;}
        virtual void createIndex(slice expressionJSON,
                                 IndexType =kValueIndex,
                                 const IndexOptions* = nullptr);
        virtual void deleteIndex(slice expressionJSON, IndexType =kValueIndex);

        // public for complicated reasons; clients should never call it
        virtual ~KeyStore()                             { }

    protected:
        KeyStore(DataFile &db, const std::string &name, Capabilities capabilities)
                :_db(db), _name(name), _capabilities(capabilities) { }

        virtual void reopen()                           { }
        virtual void close()                            { }

        virtual bool _del(slice key, Transaction&) =0;
        virtual bool _del(sequence_t s, Transaction&) =0;

        virtual RecordEnumerator::Impl* newEnumeratorImpl(slice minKey, slice maxKey,
                                                       RecordEnumerator::Options&) =0;
        virtual RecordEnumerator::Impl* newEnumeratorImpl(sequence_t min, sequence_t max,
                                                       RecordEnumerator::Options&) =0;

        DataFile &          _db;            // The DataFile I'm contained in
        const std::string   _name;          // My name
        const Capabilities  _capabilities;  // Do I support sequences or soft deletes?

    private:
        KeyStore(const KeyStore&) = delete;     // not copyable
        KeyStore& operator=(const KeyStore&) = delete;

        friend class DataFile;
        friend class RecordEnumerator;
        friend class Query;
    };

}
