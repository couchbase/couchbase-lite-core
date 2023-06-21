//
// RecordEnumerator.hh
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

#include "Record.hh"
#include <algorithm>
#include <limits.h>

namespace litecore {

    class KeyStore;

    enum SortOption {
        kDescending = -1,
        kUnsorted = 0,
        kAscending = 1
    };

    /** KeyStore enumerator/iterator that returns a range of Records.
        Usage:
            for (auto e=db.enumerate(); e.next(); ) {...}
        or
            auto e=db.enumerate();
            while (e.next()) { ... }
        Inside the loop you can treat the enumerator as though it were a Record*, for example
        "e->key()".
     */
    class RecordEnumerator {
    public:
        struct Options {
            bool           includeDeleted = false;   ///< Include deleted records?
            bool           onlyBlobs      = false;   ///< Only include records which contain linked binary data
            bool           onlyConflicts  = false;   ///< Only include records with conflicts
            SortOption     sortOption     = kAscending;    ///< Sort order, or unsorted
            ContentOption  contentOption  = kEntireBody;       ///< Load record bodies?

            Options() { }
        };

        RecordEnumerator(KeyStore&,
                         Options options = Options());
        RecordEnumerator(KeyStore&,
                         sequence_t since,
                         Options options = Options());

        RecordEnumerator(RecordEnumerator&& e) noexcept         {*this = std::move(e);}

        RecordEnumerator& operator=(RecordEnumerator&& e) noexcept {
            _store = e._store;
            _impl = std::move(e._impl);
            return *this;
        }

        /** Advances to the next key/record, returning false when it hits the end.
            next() must be called *before* accessing the first record! */
        bool next();

        /** Stops the enumerator and frees its resources. (You only need to call this if the
            destructor might not be called soon enough.) */
        void close() noexcept;

        /** True if the enumerator is at a record, false if it's at the end. */
        bool hasRecord() const FLPURE            {return _record.key().buf != nullptr;}

        /** The current record. */
        const Record& record() const FLPURE      {return _record;}

        // Can treat an enumerator as a record pointer:
        operator const Record*() const FLPURE    {return hasRecord() ? &_record : nullptr;}
        const Record* operator->() const FLPURE  {return hasRecord() ? &_record : nullptr;}

        /** Internal implementation of enumerator; each storage type must subclass it. */
        class Impl {
        public:
            virtual ~Impl()                         =default;
            virtual bool next() =0;
            virtual bool read(Record&) const =0;
            virtual slice key() const =0;
            virtual sequence_t sequence() const =0;
        };

    private:
        friend class KeyStore;

        RecordEnumerator(const RecordEnumerator&) = delete;               // no copying allowed
        RecordEnumerator& operator=(const RecordEnumerator&) = delete;    // no assignment allowed

        KeyStore *       _store;            // The KeyStore I'm enumerating
        Record           _record;           // Current record
        unique_ptr<Impl> _impl;             // The storage-specific implementation
    };

}
