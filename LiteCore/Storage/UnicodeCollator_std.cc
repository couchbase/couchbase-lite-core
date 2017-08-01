//
//  UnicodeCollator_std.cc
//  LiteCore
//
//  Created by Jens Alfke on 7/31/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "UnicodeCollator.hh"
#include "Error.hh"
#include "PlatformCompat.hh"
#include <sqlite3.h>

#include <string>
#include <codecvt>
#include <locale>
#include <iostream>

#if !__APPLE__ // See UnicodeCollator_Apple.cc for Mac/iOS/etc implementation

namespace litecore {

    using namespace std;
    using namespace fleece;


    int CompareUTF8(slice str1, slice str2, const Collation &collation) {
        error::_throw(error::Unimplemented);
    }


    int RegisterSQLiteUnicodeCollation(sqlite3* dbHandle, const Collation &collation) {
        return SQLITE_ERROR;
    }
}

#endif // !__APPLE__
