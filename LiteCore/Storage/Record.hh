//
// Record.hh
//
// Copyright 2014-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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


    /** Record's expiration timestamp: milliseconds since Unix epoch (Jan 1 1970).
        A zero value means no expiration. */
    typedef C4Timestamp expiration_t;


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
        Record()                                  =default;
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
        uint64_t subsequence() const FLPURE       {return _subsequence;}

        DocumentFlags flags() const FLPURE        {return _flags;}
        void setFlags(DocumentFlags f)            {_flags = f;}
        void setFlag(DocumentFlags f)             {_flags |= f;}
        void clearFlag(DocumentFlags f)           {_flags -= f;}

        bool exists() const FLPURE                {return _exists;}

        void setKey(slice key)                  {_key = key;}
        void setKey(alloc_slice key)            {_key = move(key);}
        void setVersion(slice vers)             {_version = vers;}
        void setVersion(alloc_slice vers)       {_version = move(vers);}

        template <class SLICE>
        void setBody(SLICE body) {
            // Leave _body alone if the new body is identical; this prevents a doc's body from
            // being swapped out when clients are using Fleece values pointing into it.
            if (slice(body) != _body || !_body) {
                _body = move(body);
                _bodySize = _body.size;
            }
        }

        template <class SLICE>
        void setExtra(SLICE extra) {
            // Same thing as setBody: there may be Fleece objects (other revs) in _extra.
            if (slice(extra) != _extra || !_extra) {
                _extra = move(extra);
                _extraSize = _extra.size;
            }
        }

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
        void updateSubsequence(uint64_t s)      {_subsequence = s;}

    private:
        friend class KeyStore;
        friend class ExclusiveTransaction;
        friend class RecordEnumerator;

        alloc_slice     _key, _version, _body, _extra;  // The key, metadata and body of the record
        size_t          _bodySize {0};          // Size of body, if body wasn't loaded
        size_t          _extraSize {0};         // Size of `extra` col, if not loaded
        sequence_t      _sequence {0};          // Sequence number (if KeyStore supports sequences)
        uint64_t        _subsequence {0};       // Per-record subsequence
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
        sequence_t      sequence {0};
        uint64_t        subsequence {0};
        DocumentFlags   flags;
    };

}
