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
#include <optional>

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

    static inline DocumentFlags& operator|= (DocumentFlags &a, DocumentFlags b) {
        return (a = a | b);
    }

    static inline DocumentFlags operator- (DocumentFlags a, DocumentFlags b) {
        return (DocumentFlags)((uint8_t)a & ~(uint8_t)b);
    }

    static inline DocumentFlags operator-= (DocumentFlags &a, DocumentFlags b) {
        return (a = a - b);
    }


    /** A Record's sequence number in a KeyStore. */
    typedef uint64_t sequence_t;


    /** Record's expiration timestamp: milliseconds since Unix epoch (Jan 1 1970).
        A zero value means no expiration. */
    typedef int64_t expiration_t;


    /** Specifies what parts of a record to read. (Used by KeyStore::get, RecordEnumerator, etc.) */
    enum ContentOption {
        kMetaOnly,          // Skip `extra` and `body`
        kCurrentRevOnly,    // Skip `extra`
        kEntireBody,        // Everything
    };


    /** The unit of storage in a DataFile: a key, version and body (all opaque blobs);
        and some extra metadata like flags and a sequence number. */
    class Record {
    public:
        Record()                                  { }
        explicit Record(slice key);
        explicit Record(alloc_slice key);

        /** Which content was loaded (set by KeyStore::get and RecordEnumerator) */
        ContentOption contentLoaded() const FLPURE {return _contentLoaded;}

        const alloc_slice& key() const FLPURE     {return _key;}
        const alloc_slice& version() const FLPURE {return _version;}
        const alloc_slice& body() const FLPURE    {return _body;}
        const alloc_slice& extra() const FLPURE   {return _extra;}

        size_t bodySize() const FLPURE            {return _bodySize;}
        size_t extraSize() const FLPURE           {return _extraSize;}

        sequence_t sequence() const FLPURE        {return _sequence;}
        sequence_t subsequence() const FLPURE     {return _subsequence;}

        DocumentFlags flags() const FLPURE        {return _flags;}
        void setFlags(DocumentFlags f)            {_flags = f;}
        void setFlag(DocumentFlags f)             {_flags |= f;}
        void clearFlag(DocumentFlags f)           {_flags -= f;}

        bool exists() const FLPURE                {return _exists;}

        template <typename T>
            void setKey(const T &key)            {_key = key;}
        template <typename T>
            void setVersion(const T &vers)       {_version = vers;}
        template <typename T>
            void setBody(const T &body)          {_body = body; _bodySize = _body.size;}
        template <typename T>
            void setExtra(const T &extra)        {_extra = extra; _extraSize = _extra.size;}

        void setKey(alloc_slice &&key)           {_key = move(key);}
        void setVersion(alloc_slice &&vers)      {_version = move(vers);}
        void setBody(alloc_slice &&body)         {_body = move(body); _bodySize = _body.size;}
        void setExtra(alloc_slice &&extra)       {_extra = move(extra); _extraSize = _extra.size;}

        uint64_t bodyAsUInt() const noexcept FLPURE;
        void setBodyAsUInt(uint64_t) noexcept;

        /** Clears/frees everything. */
        void clear() noexcept;

        void updateSequence(sequence_t s)       {_sequence = s; _subsequence = 0;}
        void updateSubsequence()                {++_subsequence;}
        void setUnloadedBodySize(size_t size)   {_body = nullslice; _bodySize = size;}
        void setUnloadedExtraSize(size_t size)  {_extra = nullslice; _extraSize = size;}
        void setExists()                        {_exists = true;}
        void setContentLoaded(ContentOption opt){_contentLoaded = opt;}

        // Only RecordEnumerator sets the expiration property
        expiration_t expiration() const FLPURE  {return _expiration;}
        void setExpiration(expiration_t x)      {_expiration = x;}

        // Only called by KeyStore
        void updateSubsequence(sequence_t s)    {_subsequence = s;}

    private:
        friend class KeyStore;
        friend class ExclusiveTransaction;
        friend class RecordEnumerator;

        alloc_slice     _key, _version, _body, _extra;  // The key, metadata and body of the record
        size_t          _bodySize {0};          // Size of body, if body wasn't loaded
        size_t          _extraSize {0};         // Size of `extra` col, if not loaded
        sequence_t      _sequence {0};          // Sequence number (if KeyStore supports sequences)
        sequence_t      _subsequence {0};       // Per-record subsequence
        expiration_t    _expiration {0};        // Expiration time (only set by RecordEnumerator)
        DocumentFlags   _flags {DocumentFlags::kNone};// Document flags (deleted, conflicted, etc.)
        bool            _exists {false};        // Does the record exist?
        ContentOption   _contentLoaded {kMetaOnly}; // Which content was loaded
    };


    /** A lightweight struct used to represent a record in KeyStore setters,
        without all the heap allocation of a Record object. */
    struct RecordUpdate {
        explicit RecordUpdate(slice key, slice body, DocumentFlags =DocumentFlags::kNone);
        explicit RecordUpdate(const Record&);
        
        slice           key, version, body, extra;
        sequence_t      sequence {0}, subsequence {0};
        DocumentFlags   flags;
    };

}
