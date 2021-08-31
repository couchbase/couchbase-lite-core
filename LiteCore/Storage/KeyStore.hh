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
#include "IndexSpec.hh"
#include "RefCounted.hh"
#include "RecordEnumerator.hh"
#include "function_ref.hh"
#include <optional>
#include <vector>

namespace litecore {

    class DataFile;
    class Record;
    class ExclusiveTransaction;
    class Query;


    enum class ReadBy {
        Key,
        Sequence
    };


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
            bool sequences;     ///< Records have sequences & can be enumerated by sequence
        };

        static constexpr Capabilities withSequences = {true};
        static constexpr Capabilities noSequences   = {false};


        DataFile& dataFile() const                  {return _db;}
        const std::string& name() const             {return _name;}
        Capabilities capabilities() const           {return _capabilities;}

        virtual uint64_t recordCount() const =0;
        virtual sequence_t lastSequence() const =0;
        virtual uint64_t purgeCount() const =0;

        virtual void erase() =0;

#if ENABLE_DELETE_KEY_STORES
        void deleteKeyStore(Transaction&);
#endif

        //////// Keys/values:

        /** Reads the rest of a record whose key() or sequence() is already set. */
        virtual bool read(Record &rec, ReadBy = ReadBy::Key, ContentOption = kEntireBody) const =0;

        Record get(slice key, ContentOption = kEntireBody) const;
        Record get(sequence_t, ContentOption = kEntireBody) const;

        using WithDocBodyCallback = function_ref<alloc_slice(const RecordUpdate&)>;

        /** Invokes the callback once for each document found in the database.
            The callback is given the docID, body and sequence, and returns a string.
            The return value is the collected strings, in the same order as the docIDs. */
        virtual std::vector<alloc_slice> withDocBodies(const std::vector<slice> &docIDs,
                                                       WithDocBodyCallback callback) =0;

        //////// Writing:

        /** Core setter for KeyStores _with_ sequences.
            The `sequence` and `subsequence` in the RecordUpdate must match the current values in
            the database, or the call will fail by returning 0 to indicate a conflict.
            (A nonexistent record's sequence and subsequence are considered to be 0.)
            @param rec  The properties of the record to save, including its _existing_ sequence
                        and subsequence.
            @param updateSequence  If true, the record's sequence will be updated to the database's
                        next consecutive sequence number. If false, the record's subsequence
                        will be incremented.
            @param transaction  The active transaction.
            @return  The record's new sequence number, or 0 if there is a conflict. */
        virtual sequence_t set(const RecordUpdate &rec,
                               bool updateSequence,
                               ExclusiveTransaction &transaction) MUST_USE_RESULT =0;

        /** Alternative `set` that takes a `Record` directly.
            It updates the `sequence` property, instead of returning the new sequence.
            It throws a Conflict exception on conflict. */
        void set(Record &rec,
                 bool updateSequence,
                 ExclusiveTransaction &transaction);

        /** Core setter for KeyStores _without_ sequences.
            There is no MVCC; whatever's stored at that key is overwritten.
            @param key  The record's key (document ID).
            @param version  Version metadata.
            @param value  The record's value (body).
            @param transaction  The current transaction. */
        virtual void setKV(slice key,
                           slice version,
                           slice value,
                           ExclusiveTransaction &transaction) =0;

        void setKV(slice key, slice value, ExclusiveTransaction &t)      {setKV(key, nullslice, value, t);}

        void setKV(Record&, ExclusiveTransaction&);

        virtual bool del(slice key, ExclusiveTransaction&, sequence_t replacingSequence =0) =0;
        bool del(const Record &rec, ExclusiveTransaction &t)                 {return del(rec.key(), t);}

        /** Sets a flag of a record, without having to read/write the Record. */
        virtual bool setDocumentFlag(slice key, sequence_t, DocumentFlags, ExclusiveTransaction&) =0;

        /** Copies record with given key to another KeyStore, with a new sequence and possibly a
            new key, then deletes it from this KeyStore. */
        virtual void moveTo(slice key, KeyStore &dst, ExclusiveTransaction&,
                            slice newKey =nullslice) =0;

        //////// Expiration:

        /** The current time represented in milliseconds since the unix epoch. */
        static expiration_t now() noexcept;

        /** Does this KeyStore potentially have records that expire? (May return false positives.) */
        virtual bool mayHaveExpiration() =0;

        /** Sets a record's expiration time. Zero means 'never'.
            @return  true if the time was set, false if no record with that key exists. */
        virtual bool setExpiration(slice key, expiration_t) =0;

        /** Returns a record's expiration time, or zero if it doesn't expire. */
        virtual expiration_t getExpiration(slice key) =0;

        /** Returns the nearest future time at which a record will expire, or 0 if none. */
        virtual expiration_t nextExpiration() =0;

        using ExpirationCallback = function_ref<void(slice docID)>;

        /** Deletes all records whose expiration time is in the past.
            @return  The number of records deleted */
        virtual unsigned expireRecords(std::optional<ExpirationCallback> =std::nullopt) =0;


        //////// Queries:

        /** A convenience that delegates to the DataFile, passing this as the defaultKeyStore. */
        Retained<Query> compileQuery(slice expr, QueryLanguage =QueryLanguage::kJSON);

        //////// Indexing:

        virtual bool supportsIndexes(IndexSpec::Type) const                   {return false;}
        virtual bool createIndex(const IndexSpec&) =0;
        virtual bool createIndex(const IndexSpec&, ExclusiveTransaction&) =0;
        bool createIndex(slice name,
                         slice expression,
                         QueryLanguage queryLanguage,
                         IndexSpec::Type =IndexSpec::kValue,
                         const IndexSpec::Options* = nullptr); // convenience method

        bool createIndex(slice name,
                         slice expression,
                         IndexSpec::Type type =IndexSpec::kValue,
                         const IndexSpec::Options* options = nullptr) // convenience method
        {
            return createIndex(name, expression, QueryLanguage::kJSON, type, options);
        }

        virtual void deleteIndex(slice name) =0;
        virtual void deleteIndex(slice name, ExclusiveTransaction &t) =0;
        virtual std::vector<IndexSpec> getIndexes() const =0;

        // public for complicated reasons; clients should never call it
        virtual ~KeyStore()                             =default;

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
