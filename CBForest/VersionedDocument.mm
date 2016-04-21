//
//  VersionedDocument.mm
//  CBForest
//
//  Created by Jens Alfke on 6/26/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include <Foundation/Foundation.h>
#include "VersionedDocument.hh"

namespace cbforest {

    VersionedDocument::VersionedDocument(KeyStore &db, NSString* docID)
    :_db(db), _doc(nsstring_slice(docID))
    {
        _db.read(_doc);
        decode();
    }

    const Revision* RevTree::get(NSString* revID) const {
        return get(revidBuffer(revID));
    }

}
