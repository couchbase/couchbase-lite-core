//
//  Record.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 11/11/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#pragma once
#include "Base.hh"

namespace litecore {

    /** The unit of storage in a DataFile: a key, metadata and body (all opaque blobs);
        and some extra metadata like a deletion flag and a sequence number. */
    class Record {
    public:
        Record()                              { }
        explicit Record(slice key);
        Record(const Record&);
        Record(Record&&) noexcept;

        const alloc_slice& key() const          {return _key;}
        const alloc_slice& meta() const         {return _meta;}
        const alloc_slice& body() const         {return _body;}

        size_t bodySize() const                 {return _bodySize;}

        sequence_t sequence() const             {return _sequence;}
        bool deleted() const                    {return _deleted;}

        /** A storage-system-dependent position in the database file, that can be used later
            to retrieve the record. Not supported by all storage systems. */
        uint64_t offset() const                 {return _offset;}

        bool exists() const                     {return _exists;}

        template <typename T>
            void setKey(const T &key)           {_key = key;}
        template <typename T>
            void setMeta(const T &meta)         {_meta = meta;}
        template <typename T>
            void setBody(const T &body)         {_body = body; _bodySize = _body.size;}

        void setDeleted(bool deleted)           {_deleted = deleted; if (deleted) _exists = false;}

        /** Reallocs the 'meta' slice to the desired size. */
        const alloc_slice& resizeMeta(size_t newSize)        {_meta.resize(newSize); return _meta;}

        /** Clears/frees everything. */
        void clear() noexcept;

        /** Clears everything but the key. */
        void clearMetaAndBody() noexcept;

        void updateSequence(sequence_t s)       {_sequence = s;}
        void setUnloadedBodySize(size_t size)   {_body = nullslice; _bodySize = size;}

        uint64_t bodyAsUInt() const noexcept;
        void setBodyAsUInt(uint64_t) noexcept;

    private:
        friend class KeyStore;
        friend class KeyStoreWriter;
        friend class Transaction;
        friend class RecordEnumerator;

        void update(sequence_t sequence, uint64_t offset, bool deleted) {
            _sequence = sequence; _offset = offset; _deleted = deleted; _exists = !deleted;
        }

        alloc_slice _key, _meta, _body;     // The key, metadata and body of the record
        size_t      _bodySize {0};          // Size of body, if body wasn't loaded
        sequence_t  _sequence {0};          // Sequence number (if KeyStore supports sequences)
        uint64_t    _offset {0};            // File offset in db, if KeyStore supports that
        bool        _deleted {false};       // Is the record deleted?
        bool        _exists {false};        // Does the record exist?
    };

}
