//
// UnicodeCollator_Stub.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include "UnicodeCollator.hh"
#include "Error.hh"

#if !__APPLE__ && !defined(_MSC_VER) && !LITECORE_USES_ICU  // Stub implementation for when collation is unavailable

namespace litecore {

    using namespace std;

    int CompareUTF8(slice str1, slice str2, const Collation &coll) {
        error::_throw(error::Unimplemented);
    }

    unique_ptr<CollationContext> RegisterSQLiteUnicodeCollation(sqlite3* dbHandle,
                                                                const Collation &coll) {
        return nullptr;
    }

    int ContainsUTF8(fleece::slice str, fleece::slice substr, const Collation& coll) {
        auto current = substr;
        while(str.size > 0) {
            size_t nextStrSize = NextUTF8Length(str);
            size_t nextSubstrSize = NextUTF8Length(current);
            if(!CompareUTF8({str.buf, nextStrSize}, {current.buf, nextSubstrSize}, coll)) {
                // The characters are a match, move to the next substring character
                current.moveStart(nextSubstrSize);
                if(current.size == 0) {
                    // Found a match!
                    return 0;
                }
            } else {
                current = substr;
            }

            str.moveStart(nextStrSize);
        }
        
        return -1;
    }
}

#endif
