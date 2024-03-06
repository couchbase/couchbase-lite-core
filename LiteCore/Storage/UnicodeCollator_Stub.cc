//
// UnicodeCollator_Stub.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "UnicodeCollator.hh"
#include "Error.hh"

#if !__APPLE__ && !defined(_MSC_VER) && !LITECORE_USES_ICU  // Stub implementation for when collation is unavailable

namespace litecore {

    using namespace std;

    int CompareUTF8(fleece::slice str1, fleece::slice str2, const CollationContext&) {
        error::_throw(error::Unimplemented);
    }

    bool ContainsUTF8(fleece::slice str, fleece::slice substr, const CollationContext&) {
        error::_throw(error::Unimplemented);
    }

    unique_ptr<CollationContext> RegisterSQLiteUnicodeCollation(sqlite3* dbHandle, const Collation& coll) {
        return nullptr;
    }

    unique_ptr<CollationContext> CollationContext::create(const Collation& coll) {
        error::_throw(error::Unimplemented);
    }

}  // namespace litecore

#endif
