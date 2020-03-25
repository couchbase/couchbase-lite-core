//
// SQLiteChooser.c
//
// Copyright (c) 2018 Couchbase, Inc All rights reserved.
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

// Compiles the appropriate version of SQLite, depending on whether we are building the
// Consumer Edition or Enterprise Edition of LiteCore.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

#ifdef COUCHBASE_ENTERPRISE
    // These source files are NOT in this repository, rather in a Couchbase-private
    // repository that needs to be checked out next to this one.
    #ifndef SQLITE_HAS_CODEC
        #error SQLITE_HAS_CODEC was not defined in EE build
    #endif
    #include "../../../couchbase-lite-core-EE/Encryption/see-sqlite.c"
    #if __APPLE__
        #define CCCRYPT256
        #include "../../../couchbase-lite-core-EE/Encryption/see-cccrypt.c"
    #else
        #include "../../../couchbase-lite-core-EE/Encryption/see-aes256-ofb.c"
    #endif
#else
    #include "../../vendor/SQLiteCpp/sqlite3/sqlite3.c"
#endif

#pragma GCC diagnostic pop
