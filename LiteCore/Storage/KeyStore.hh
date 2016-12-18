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
#include <functional>

namespace litecore {

    class DataFile;
    class Record;
    class Transaction;
    class Query;

    /** A sequence number in a KeyStore. */
    typedef uint64_t sequence;
    typedef uint64_t docOffset;

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
            bool softDeletes    :1;     ///< Deleted records have sequence numbers (until compact)
            bool getByOffset    :1;     ///< getByOffset can retrieve overwritten docs

            static const Capabilities defaults;
        };


        DataFile& dataFile() const                  {return _db;}
        const std::string& name() const             {return _name;}
        Capabilities capabilities() const           {return _capabilities;}

        virtual uint64_t recordCount() const =0;
        virtual sequence lastSequence() const =0;

        virtual void erase() =0;

        void deleteKeyStore(Transaction&);

        // Keys/values:

        Record get(slice key, ContentOptions = kDefaultContent) const;
        virtual Record get(sequence, ContentOptions = kDefaultContent) const =0;

        virtual void get(slice key, ContentOptions, std::function<void(const Record&)>);
        virtual void get(sequence, ContentOptions, std::function<void(const Record&)>);

        /** Reads a record whose key() is already set. */
        virtual bool read(Record &rec, ContentOptions options = kDefaultContent) const =0;

        /** Reads the body of a Record that's already been read with kMetaonly.
            Does nothing if the record's body is non-null. */
        virtual void readBody(Record &rec) const;

        virtual Record getByOffsetNoErrors(docOffset, sequence) const
                {return Record();}

        /** Creates a database query object. */
        virtual Query* compileQuery(slice expr);

        //////// Writing:

        struct setResult {sequence seq; docOffset off;};

        virtual setResult set(slice key, slice meta, slice value, Transaction&) =0;
        setResult set(slice key, slice value, Transaction &t)
                                                        {return set(key, nullslice, value, t);}
        void write(Record&, Transaction&);

        bool del(slice key, Transaction&);
        bool del(sequence s, Transaction&);
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
        virtual void createIndex(const std::string &propertyPath,
                                 IndexType =kValueIndex,
                                 const IndexOptions* = nullptr);
        virtual void deleteIndex(const std::string &propertyPath, IndexType =kValueIndex);

        // public for complicated reasons; clients should never call it
        virtual ~KeyStore()                             { }

    protected:
        KeyStore(DataFile &db, const std::string &name, Capabilities capabilities)
                :_db(db), _name(name), _capabilities(capabilities) { }

        virtual void reopen()                           { }
        virtual void close()                            { }

        virtual bool _del(slice key, Transaction&) =0;
        virtual bool _del(sequence s, Transaction&) =0;

        virtual RecordEnumerator::Impl* newEnumeratorImpl(slice minKey, slice maxKey,
                                                       RecordEnumerator::Options&) =0;
        virtual RecordEnumerator::Impl* newEnumeratorImpl(sequence min, sequence max,
                                                       RecordEnumerator::Options&) =0;

        void updateDoc(Record &rec, sequence seq, docOffset offset =0, bool deleted = false) const {
            rec.update(seq, offset, deleted);
        }

        DataFile &          _db;            // The DataFile I'm contained in
        const std::string   _name;          // My name
        const Capabilities  _capabilities;  // Do I support sequences or soft deletes?

    private:
        KeyStore(const KeyStore&) = delete;     // not copyable
        KeyStore& operator=(const KeyStore&) = delete;

        friend class DataFile;
        friend class RecordEnumerator;
        friend class KeyStoreWriter;
        friend class Query;
    };

}
