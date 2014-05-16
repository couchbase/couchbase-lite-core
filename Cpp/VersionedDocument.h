//
//  VersionedDocument.h
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__VersionedDocument__
#define __CBForest__VersionedDocument__
#include "RevTree.h"

namespace forestdb {

    class VersionedDocument : public RevTree {
    public:

        typedef uint8_t Flags;
        enum {
            kDeleted    = 0x01,
            kConflicted = 0x02,
        };

        VersionedDocument(Database* db, slice docID);
        VersionedDocument(Database* db, Document* doc);
        ~VersionedDocument();

        slice docID() const         {return _doc->key();}
        slice revID() const;
        Flags flags() const;

        Document* document();

        alloc_slice readBodyOfNode(const RevNode*);

        bool changed() const        {return _changed;}
        void save(Transaction& transaction);

    private:
        Database* _db;
        Document* _doc;
        bool _freeDoc;
    };
}

#endif /* defined(__CBForest__VersionedDocument__) */
