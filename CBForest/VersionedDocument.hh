//
//  VersionedDocument.hh
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#ifndef __CBForest__VersionedDocument__
#define __CBForest__VersionedDocument__
#include "RevTree.hh"
#include "Document.hh"

namespace forestdb {

    /** Manages storage of a serialized RevTree in a Document. */
    class VersionedDocument : public RevTree {
    public:

        /** Flags that apply to the document as a whole */
        typedef uint8_t Flags;
        enum {
            kDeleted    = 0x01,
            kConflicted = 0x02,
            kHasAttachments = 0x04
        };

        VersionedDocument(Database* db, slice docID);
        VersionedDocument(Database* db, const Document&);
        VersionedDocument(Database* db, Document&&);

#ifdef __OBJC__
        VersionedDocument(Database* db, NSString* docID);
#endif

        /** Reads and parses the body of the document. Useful if doc was read as meta-only. */
        void read();

        /** Returns false if the document was loaded metadata-only. Revision accessors will fail. */
        bool revsAvailable() const {return !_unknown;}

        slice docID() const         {return _doc.key();}
        revid revID() const;
        Flags flags() const;
        bool isDeleted() const      {return (flags() & kDeleted) != 0;}
        bool isConflicted() const   {return (flags() & kConflicted) != 0;}
        bool hasAttachments() const {return (flags() & kHasAttachments) != 0;}

        bool exists() const         {return _doc.exists();}
        sequence sequence() const   {return _doc.sequence();}

        bool changed() const        {return _changed;}
        void save(Transaction& transaction);

        /** Gets the flags from a document without having to instantiate a VersionedDocument */
        static Flags flagsOfDocument(const Document&);

#if DEBUG
        std::string dump()          {return RevTree::dump();}
#endif
    protected:
        virtual bool isBodyOfRevisionAvailable(const Revision*, uint64_t atOffset) const;
        virtual alloc_slice readBodyOfRevision(const Revision*, uint64_t atOffset) const;
#if DEBUG
        virtual void dump(std::ostream&);
#endif

    private:
        void decode();
        void updateMeta();
        VersionedDocument(const VersionedDocument&); // forbidden

        Database* _db;
        Document _doc;
    };
}

#endif /* defined(__CBForest__VersionedDocument__) */
