//
// KeyStore.hh
//
// Copyright (c) 2014 Couchbase, Inc All rights reserved.
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
#include "Base.hh"
#include "RefCounted.hh"
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

#if ENABLE_DELETE_KEY_STORES
        void deleteKeyStore(Transaction&);
#endif

        // Keys/values:

        Record get(slice key, ContentOptions = kDefaultContent) const;
        virtual Record get(sequence_t) const =0;

        virtual void get(slice key, ContentOptions, function_ref<void(const Record&)>);
        virtual void get(sequence_t, function_ref<void(const Record&)>);

        /** Reads a record whose key() is already set. */
        virtual bool read(Record &rec, ContentOptions options = kDefaultContent) const =0;

        /** Reads the body of a Record that's already been read with kMetaonly.
            Does nothing if the record's body is non-null. */
        virtual void readBody(Record &rec) const;

        /** Creates a database query object. */
        virtual Retained<Query> compileQuery(slice expr);

        //////// Writing:

        /** Core write method. If replacingSequence is not null, will only update the
            record if its existing sequence matches. (Or if the record doesn't already
            exist, in the case where *replacingSequence == 0.) */
        virtual sequence_t set(slice key, slice version, slice value,
                               DocumentFlags,
                               Transaction&,
                               const sequence_t *replacingSequence =nullptr,
                               bool newSequence =true) =0;

        sequence_t set(slice key, slice value, Transaction &t,
                       const sequence_t *replacingSequence =nullptr) {
            return set(key, nullslice, value, DocumentFlags::kNone, t, replacingSequence);
        }

        void write(Record&, Transaction&, const sequence_t *replacingSequence =nullptr);

        virtual bool del(slice key, Transaction&, sequence_t replacingSequence =0) =0;
        bool del(const Record &rec, Transaction &t)                 {return del(rec.key(), t);}

        /** Sets a flag of a record, without having to read/write the Record. */
        virtual bool setDocumentFlag(slice key, sequence_t, DocumentFlags, Transaction&);

        //////// INDEXING:

        enum IndexType {
            kValueIndex,         ///< Regular index of property value
            kFullTextIndex,      ///< Full-text index
            kGeoIndex,           ///< Geo index of GeoJSON values
        };

        struct IndexOptions {
            const char *language;   ///< NULL or an ISO language code ("en", etc)
            bool ignoreDiacritics;  ///< True to strip diacritical marks/accents from letters
            bool disableStemming;   ///< Disables stemming
            const char *stopWords;  ///< NULL for default, or comma-delimited string, or empty
        };

        virtual bool supportsIndexes(IndexType) const                   {return false;}
        virtual void createIndex(slice name,
                                 slice expressionJSON,
                                 IndexType =kValueIndex,
                                 const IndexOptions* = nullptr);
        virtual void deleteIndex(slice name);
        virtual alloc_slice getIndexes() const;

        // public for complicated reasons; clients should never call it
        virtual ~KeyStore()                             { }

    protected:
        KeyStore(DataFile &db, const std::string &name, Capabilities capabilities)
                :_db(db), _name(name), _capabilities(capabilities) { }

        virtual void reopen()                           { }
        virtual void close()                            { }

        virtual RecordEnumerator::Impl* newEnumeratorImpl(bool bySequence,
                                                          sequence_t since,
                                                          RecordEnumerator::Options) =0;

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
