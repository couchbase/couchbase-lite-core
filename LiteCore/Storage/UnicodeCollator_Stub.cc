//
//  UnicodeCollator_Stub.cc
//  LiteCore
//
//  Created by Jens Alfke on 8/2/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "UnicodeCollator.hh"
#include "Error.hh"

#if !__APPLE__ && !LITECORE_USES_ICU  // Stub implementation for when collation is unavailable

namespace litecore {

    using namespace std;

    int CompareUTF8(slice str1, slice str2, const Collation &coll) {
        error::_throw(error::Unimplemented);
    }

    unique_ptr<CollationContext> RegisterSQLiteUnicodeCollation(sqlite3* dbHandle,
                                                                const Collation &coll) {
        return nullptr;
    }
}

#endif
