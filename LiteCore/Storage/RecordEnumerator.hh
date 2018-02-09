//
// RecordEnumerator.hh
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

#include "Record.hh"
#include <limits.h>
#include <vector>

namespace litecore {

    class KeyStore;

    enum ContentOptions {
        kDefaultContent = 0,
        kMetaOnly = 0x01
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
            bool           descending     :1;   ///< Reverse order? (Start must be
            bool           includeDeleted :1;   ///< Include deleted records?
            bool           onlyBlobs      :1;   ///< Only include records which contain linked binary data
            ContentOptions contentOptions :4;   ///< Load record bodies?

            /** Default options have all flags false, and kDefaultContent */
            Options();
        };

        RecordEnumerator(KeyStore&,
                         Options options = Options());
        RecordEnumerator(KeyStore&,
                         sequence_t since,
                         Options options = Options());

        RecordEnumerator(RecordEnumerator&& e) noexcept         {*this = std::move(e);}

        RecordEnumerator& operator=(RecordEnumerator&& e) noexcept {
            _store = e._store;
            _impl = move(e._impl);
            return *this;
        }

        /** Advances to the next key/record, returning false when it hits the end.
            next() must be called *before* accessing the first record! */
        bool next();

        /** Stops the enumerator and frees its resources. (You only need to call this if the
            destructor might not be called soon enough.) */
        void close() noexcept;

        /** The current record. */
        const Record& record() const      {return _record;}

        // Can treat an enumerator as a record pointer:
        operator const Record*() const    {return _record.key().buf ? &_record : nullptr;}
        const Record* operator->() const  {return _record.key().buf ? &_record : nullptr;}

        /** Internal implementation of enumerator; each storage type must subclass it. */
        class Impl {
        public:
            virtual ~Impl()                         { }
            virtual bool next() =0;
            virtual bool read(Record&) =0;
        };

    private:
        friend class KeyStore;

        RecordEnumerator(const RecordEnumerator&) = delete;               // no copying allowed
        RecordEnumerator& operator=(const RecordEnumerator&) = delete;    // no assignment allowed

        KeyStore *      _store;             // The KeyStore I'm enumerating
        Record          _record;            // Current record
        std::unique_ptr<Impl> _impl;        // The storage-specific implementation
    };

}
