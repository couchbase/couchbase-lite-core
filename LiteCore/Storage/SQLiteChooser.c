//
//  SQLiteChooser.c
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 2/12/18.
//  Copyright © 2018 Couchbase. All rights reserved.
//

// Compiles the appropriate version of SQLite, depending on whether we are building the
// Consumer Edition or Enterprise Edition of LiteCore.

#ifdef COUCHBASE_ENTERPRISE
    // This source file is NOT in this repository, rather in a private repository that needs to be
    // checked out next to this one.
    #include "../../../couchbase-lite-core-EE/Encryption/see-sqlite.c"
#else
    #include "../../vendor/SQLiteCpp/sqlite3/sqlite3.c"
#endif
