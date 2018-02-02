//
// Record.hh
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

namespace litecore {

    /** Flags used by Document, stored in a Record. Matches C4DocumentFlags. */
    enum class DocumentFlags : uint8_t {
        kNone           = 0x00,
        kDeleted        = 0x01, ///< Document's current revision is deleted (a tombstone)
        kConflicted     = 0x02, ///< Document is in conflict (multiple leaf revisions)
        kHasAttachments = 0x04, ///< Document has one or more revisions with attachments/blobs
        kSynced         = 0x08, ///< Document's current revision has been pushed to server
    };

    static inline bool operator& (DocumentFlags a, DocumentFlags b) {
        return ((uint8_t)a & (uint8_t)b) != 0;
    }

    static inline DocumentFlags operator| (DocumentFlags a, DocumentFlags b) {
        return (DocumentFlags)((uint8_t)a | (uint8_t)b);
    }

    /** The unit of storage in a DataFile: a key, version and body (all opaque blobs);
        and some extra metadata like flags and a sequence number. */
    class Record {
    public:
        Record()                              { }
        explicit Record(slice key);
        Record(const Record&);
        Record(Record&&) noexcept;

        const alloc_slice& key() const          {return _key;}
        const alloc_slice& version() const      {return _version;}
        const alloc_slice& body() const         {return _body;}

        size_t bodySize() const                 {return _bodySize;}

        sequence_t sequence() const             {return _sequence;}

        DocumentFlags flags() const             {return _flags;}
        void setFlags(DocumentFlags f)          {_flags = f;}
        void setFlag(DocumentFlags f)           {_flags = (DocumentFlags)((uint8_t)_flags | (uint8_t)f);}
        void clearFlag(DocumentFlags f)         {_flags = (DocumentFlags)((uint8_t)_flags & ~(uint8_t)f);}

        bool exists() const                     {return _exists;}

        template <typename T>
            void setKey(const T &key)           {_key = key;}
        template <typename T>
            void setVersion(const T &vers)      {_version = vers;}
        template <typename T>
            void setBody(const T &body)         {_body = body; _bodySize = _body.size;}

        uint64_t bodyAsUInt() const noexcept;
        void setBodyAsUInt(uint64_t) noexcept;

        /** Clears/frees everything. */
        void clear() noexcept;

        /** Clears everything but the key. */
        void clearMetaAndBody() noexcept;

        void updateSequence(sequence_t s)       {_sequence = s;}
        void setUnloadedBodySize(size_t size)   {_body = nullslice; _bodySize = size;}
        void setExists()                        {_exists = true;}

    private:
        friend class KeyStore;
        friend class Transaction;
        friend class RecordEnumerator;

        alloc_slice     _key, _version, _body;  // The key, metadata and body of the record
        size_t          _bodySize {0};          // Size of body, if body wasn't loaded
        sequence_t      _sequence {0};          // Sequence number (if KeyStore supports sequences)
        DocumentFlags   _flags {DocumentFlags::kNone};// Document flags (deleted, conflicted, etc.)
        bool            _exists {false};        // Does the record exist?
    };

}
