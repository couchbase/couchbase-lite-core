//
//  RecordEnumerator.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 6/18/14.
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

            /** Default options have inclusiveStart, inclusiveEnd, and include bodies. */
            Options();
        };

        RecordEnumerator(KeyStore&,
                         const Options& options = Options());
        RecordEnumerator(KeyStore&,
                         sequence_t since,
                         const Options& options = Options());

        RecordEnumerator(RecordEnumerator&& e) noexcept    :_store(e._store) {*this = std::move(e);}
        RecordEnumerator& operator=(RecordEnumerator&& e) noexcept; // move assignment

        /** Advances to the next key/record, returning false when it hits the end.
            next() must be called *before* accessing the first record! */
        bool next();

        bool atEnd() const noexcept         {return !_record.key();}

        /** Stops the enumerator and frees its resources. (You only need to call this if the
            destructor might not be called soon enough.) */
        void close() noexcept;

        /** The current record. */
        const Record& record() const         {return _record;}

        // Can treat an enumerator as a record pointer:
        operator const Record*() const    {return _record.key().buf ? &_record : nullptr;}
        const Record* operator->() const  {return _record.key().buf ? &_record : nullptr;}

        RecordEnumerator(const RecordEnumerator&) = delete;               // no copying allowed
        RecordEnumerator& operator=(const RecordEnumerator&) = delete;    // no assignment allowed

        // C++11 'for' loop support
        // Allows a RecordEnumerator `e` to be used like:
        //    for (const Record &record : e) { ... }
        struct Iter {
            RecordEnumerator* const _enum;
            Iter& operator++ ()             {_enum->next(); return *this;}
            operator const Record* ()     {return _enum ? (const Record*)(*_enum) : nullptr;}
        };
        Iter begin() noexcept    {next(); return Iter{this};}
        Iter end() noexcept      {return Iter{nullptr};}

        /** Internal implementation of enumerator; each storage type must subclass it. */
        class Impl {
        public:
            virtual ~Impl()                         { }
            virtual bool next() =0;
            virtual bool read(Record&) =0;
            virtual bool shouldSkipFirstStep()      {return false;}
        };

    private:
        friend class KeyStore;

        RecordEnumerator(KeyStore &store, const Options& options, bool);
        void initialPosition();
        bool nextFromArray();
        bool getDoc();

        KeyStore *      _store;             // The KeyStore I'm enumerating
        Options         _options;           // Enumeration options
        Record          _record;            // Current record
        bool            _skipStep {false};  // Should next call to next() skip _impl->next()?
        std::unique_ptr<Impl> _impl;        // The storage-specific implementation
    };

}
