//
// cbliteTool+sql.cc
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

#include "cbliteTool.hh"
#include "c4Private.h"


void CBLiteTool::sqlUsage() {
    writeUsageCommand("sql", false, "QUERY");
    cerr <<
    "  Runs a raw SQL query on the database file. This is NOT a way to query your documents!\n"
    "  Rather, it's a very low-level diagnostic tool that will not be useful unless you know the\n"
    "  underlying SQLite schema used by LiteCore.\n"
    "    NOTE: Query must be a single (quoted) argument. Sorry.\n"
    ;
}


void CBLiteTool::sqlQuery() {
    // Read params:
    if (_showHelp) {
        sqlUsage();
        return;
    }
    openDatabaseFromNextArg();
    string sql = nextArg("sql statement");
    if (argCount() > 0)
        fail("Sorry, the entire SQL command needs be \"quoted\".");

    C4Error error;
    alloc_slice fleeceResult = c4db_rawQuery(_db, slice(sql), &error);
    if (!fleeceResult)
        fail("Query failed", error);

    prettyPrint(Value::fromData(fleeceResult));
    cout << '\n';
}

