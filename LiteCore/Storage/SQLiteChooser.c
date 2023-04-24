//
// SQLiteChooser.c
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

// Compiles the appropriate version of SQLite, depending on whether we are building the
// Consumer Edition or Enterprise Edition of LiteCore.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

#ifdef COUCHBASE_ENTERPRISE
// These source files are NOT in this repository, rather in a Couchbase-private
// repository that needs to be checked out next to this one.
#    ifndef SQLITE_HAS_CODEC
#        error SQLITE_HAS_CODEC was not defined in EE build
#    endif
#    if __APPLE__
#        define CCCRYPT256
#        include "../../../couchbase-lite-core-EE/Encryption/sqlite3-see-cccrypt.c"
#    else
#        include "../../../couchbase-lite-core-EE/Encryption/sqlite3-see-aes256-ofb.c"
#    endif
#else
#    include "../../vendor/SQLiteCpp/sqlite3/sqlite3.c"
#endif

#pragma GCC diagnostic pop
