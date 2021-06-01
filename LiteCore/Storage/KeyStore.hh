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
    class Transaction;
    class Query;


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
        virtual uint64_t purgeCount() const =0;

        virtual void erase() =0;

#if ENABLE_DELETE_KEY_STORES
        void deleteKeyStore(Transaction&);
#endif

        //////// Keys/values:

        Record get(slice key, ContentOption = kEntireBody) const;
        virtual Record get(sequence_t, ContentOption = kEntireBody) const =0;

        virtual void get(slice key, ContentOption, function_ref<void(const Record&)>);

        /** Reads a record whose key() is already set. */
        virtual bool read(Record &rec, ContentOption = kEntireBody) const =0;

        /** Creates a database query object. */
        virtual Retained<Query> compileQuery(slice expr, QueryLanguage =QueryLanguage::kJSON) =0;

        using WithDocBodyCallback = function_ref<alloc_slice(const RecordLite&)>;

        /** Invokes the callback once for each document found in the database.
            The callback is given the docID, body and sequence, and returns a string.
            The return value is the collected strings, in the same order as the docIDs. */
        virtual std::vector<alloc_slice> withDocBodies(const std::vector<slice> &docIDs,
                                                       WithDocBodyCallback callback) =0;

        //////// Writing:

        /** Core write method.
            If `rec.sequence` is not `nullopt`, the record will not be updated if its existing sequence
            doesn't match it. (A nonexistent record's "existing sequence" is considered to be 0.)
            If `rec.updateSequence` is false, the record's sequence won't be changed, but its
            current sequence must be provided in `rec.sequence`.
            Returns the record's new sequence, or 0 if the record was not updated due to a sequence
            conflict. */
        virtual sequence_t set(const RecordLite &rec, Transaction&) =0;

        // Convenience wrappers for set():

        sequence_t set(slice key, slice version, slice value,
                       DocumentFlags flags,
                       Transaction &t,
                       std::optional<sequence_t> replacingSequence =std::nullopt,
                       bool newSequence =true)
        {
            RecordLite r = {key, version, value, nullslice, replacingSequence, newSequence, flags};
            return set(r, t);
        }

        sequence_t set(slice key, slice value, Transaction &t,
                       std::optional<sequence_t> replacingSequence =std::nullopt,
                       bool newSequence =true) {
            RecordLite r = {key, nullslice, value, nullslice,
                              replacingSequence, newSequence, DocumentFlags::kNone};
            return set(r, t);
        }

        sequence_t set(Record&,
                       Transaction&,
                       std::optional<sequence_t> replacingSequence =std::nullopt,
                       bool newSequence =true);

        virtual bool del(slice key, Transaction&, sequence_t replacingSequence =0) =0;
        bool del(const Record &rec, Transaction &t)                 {return del(rec.key(), t);}

        /** Sets a flag of a record, without having to read/write the Record. */
        virtual bool setDocumentFlag(slice key, sequence_t, DocumentFlags, Transaction&) =0;


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


        //////// Indexing:

        virtual bool supportsIndexes(IndexSpec::Type) const                   {return false;}
        virtual bool createIndex(const IndexSpec&) =0;
        bool createIndex(slice name,
                         slice expression,
                         QueryLanguage queryLanguage,
                         IndexSpec::Type =IndexSpec::kValue,
                         const IndexSpec::Options* = nullptr); // convenience method
        inline bool createIndex(slice name,
                                slice expressionJSON,
                                IndexSpec::Type indexType=IndexSpec::kValue,
                                const IndexSpec::Options* indexOption =nullptr) // convenience method
        {
            return createIndex(name, expressionJSON, QueryLanguage::kJSON, indexType, indexOption);
        }
        virtual void deleteIndex(slice name) =0;
        virtual std::vector<IndexSpec> getIndexes() const =0;

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
