//
//  VersionedDocument.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/14/14.
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
#include "RevTree.hh"
#include "Record.hh"

namespace litecore {

    /** Manages storage of a serialized RevTree in a Record. */
    class VersionedDocument : public RevTree {
    public:

        /** Flags that apply to the record as a whole */
        typedef uint8_t Flags;
        enum {
            kDeleted    = 0x01,
            kConflicted = 0x02,
            kHasAttachments = 0x04
        };

        VersionedDocument(KeyStore&, slice docID);
        VersionedDocument(KeyStore&, const Record&);

#ifdef __OBJC__
        VersionedDocument(KeyStore&, NSString* docID);
#endif

        /** Reads and parses the body of the record. Useful if doc was read as meta-only. */
        void read();

        /** Returns false if the record was loaded metadata-only. Revision accessors will fail. */
        bool revsAvailable() const {return !_unknown;}

        const alloc_slice& docID() const {return _rec.key();}
        revid revID() const         {return _revID;}
        Flags flags() const         {return _flags;}
        bool isDeleted() const      {return (flags() & kDeleted) != 0;}
        bool isConflicted() const   {return (flags() & kConflicted) != 0;}
        bool hasAttachments() const {return (flags() & kHasAttachments) != 0;}

        bool exists() const         {return _rec.exists();}
        sequence_t sequence() const {return _rec.sequence();}

        const Record& record() const    {return _rec;}

        slice docType() const       {return _recType;}
        void setDocType(slice type) {_recType = type;}

        bool changed() const        {return _changed;}
        void save(Transaction& transaction);

        /** Gets the metadata of a record without having to instantiate a VersionedDocument */
        static bool readMeta(const Record&, Flags&, revid&, slice& docType);

        void updateMeta();

#if DEBUG
        std::string dump()          {return RevTree::dump();}
#endif
    protected:
        virtual bool isBodyOfRevisionAvailable(const Rev*, uint64_t atOffset) const override;
        virtual alloc_slice readBodyOfRevision(const Rev*, uint64_t atOffset) const override;
#if DEBUG
        virtual void dump(std::ostream&) override;
#endif

    private:
        void decode();
        VersionedDocument(const VersionedDocument&) = delete;

        KeyStore&   _db;
        Record    _rec;
        Flags       _flags;
        revid       _revID;
        alloc_slice _recType;
    };
}
