//
//  cbliteTool+sql.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/8/17.
//  Copyright © 2017 Couchbase. All rights reserved.
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

