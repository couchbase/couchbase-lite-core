//
//  VersionedDocument.mm
//  CBForest
//
//  Created by Jens Alfke on 6/26/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include <Foundation/Foundation.h>
#include "VersionedDocument.hh"

namespace forestdb {

    VersionedDocument::VersionedDocument(Database* db, NSString* docID)
    :_db(db), _doc(nsstring_slice(docID))
    {
        _db->read(_doc);
        decode();
    }

    const Revision* RevTree::get(NSString* revID) const {
        return get(revidBuffer(revID));
    }

}
