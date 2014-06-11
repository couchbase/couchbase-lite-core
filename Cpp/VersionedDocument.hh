//
//  VersionedDocument.h
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__VersionedDocument__
#define __CBForest__VersionedDocument__
#include "RevTree.hh"

namespace forestdb {

    class VersionedDocument : public RevTree {
    public:

        /** Flags that apply to the document as a whole */
        typedef uint8_t Flags;
        enum {
            kDeleted    = 0x01,
            kConflicted = 0x02,
        };

        VersionedDocument(Database* db, slice docID);
        VersionedDocument(Database* db, const Document& doc);
        VersionedDocument(Database* db, Document&& doc);

        slice docID() const         {return _doc.key();}
        revid revID() const;
        Flags flags() const;
        bool isDeleted() const      {return (flags() & kDeleted) != 0;}

        bool exists() const         {return _doc.exists();}
        sequence sequence() const   {return _doc.sequence();}

        Document& document();

        bool changed() const        {return _changed;}
        void save(Transaction& transaction);

    protected:
        virtual bool isBodyOfNodeAvailable(const RevNode* node) const;
        virtual alloc_slice readBodyOfNode(const RevNode*) const;

    private:
        Database* _db;
        Document _doc;
    };
}

#endif /* defined(__CBForest__VersionedDocument__) */
