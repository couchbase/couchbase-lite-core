//
//  cbliteTool+Query.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/8/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "cbliteTool.hh"


void CBLiteTool::queryUsage() {
    writeUsageCommand("query", true, "JSONQUERY");
    cerr <<
    "  Runs a query against the database."
    "    --offset N : Skip first N rows\n"
    "    --limit N : Stop after N rows\n"
    "    " << it("JSONQUERY") << " : LiteCore JSON (or JSON5) query expression\n"
    ;
}


void CBLiteTool::queryDatabase() {
    // Read params:
    processFlags(kQueryFlags);
    if (_showHelp) {
        queryUsage();
        return;
    }
    openDatabaseFromNextArg();
    alloc_slice queryJSON = convertQuery(nextArg("query string"));
    endOfArgs();

    // Compile query:
    C4Error error;
    c4::ref<C4Query> query = c4query_new(_db, queryJSON, &error);
    if (!query)
        fail("compiling query", error);
    bool columnsSpecified = (c4query_columnCount(query) > 0);

    // Set parameters:
    alloc_slice params;
    if (_offset > 0 || _limit >= 0) {
        JSONEncoder enc;
        enc.beginDict();
        enc.writeKey("offset"_sl);
        enc.writeInt(_offset);
        enc.writeKey("limit"_sl);
        enc.writeInt(_limit);
        enc.endDict();
        params = enc.finish();
    }

    // Run query:
    c4::ref<C4QueryEnumerator> e = c4query_run(query, nullptr, params, &error);
    if (!e)
        fail("starting query", error);
    if (_offset > 0)
        cout << "(Skipping first " << _offset << " rows)\n";
    uint64_t nRows = 0;
    while (c4queryenum_next(e, &error)) {
        // Write a result row:
        ++nRows;
        cout << "[";
        int nCols = 0;
        if (!columnsSpecified) {
            cout << "\"_id\": \"" << e->docID << "\"";
            ++nCols;
        }
        for (Array::iterator i(e->columns); i; ++i) {
            if (nCols++)
                cout << ", ";
            alloc_slice json = i.value().toJSON();
            cout << json;
        }
        cout << "]\n";
    }
    if (error.code)
        fail("running query", error);
    if (nRows == _limit)
        cout << "(Limit was " << _limit << " rows)\n";
}


alloc_slice CBLiteTool::convertQuery(slice inputQuery) {
    FLError flErr;
    alloc_slice queryJSONBuf = FLJSON5_ToJSON(slice(inputQuery), &flErr);
    if (!queryJSONBuf)
        fail("Invalid JSON in query");

    // Trim whitespace from either end:
    slice queryJSON = queryJSONBuf;
    while (isspace(queryJSON[0]))
        queryJSON.moveStart(1);
    while (isspace(queryJSON[queryJSON.size-1]))
        queryJSON.setSize(queryJSON.size-1);

    stringstream json;
    if (queryJSON[0] == '[')
        json << "{\"WHERE\": " << queryJSON;
    else
        json << slice(queryJSON.buf, queryJSON.size - 1);
    if (_offset > 0 || _limit >= 0)
        json << ", \"OFFSET\": [\"$offset\"], \"LIMIT\":  [\"$limit\"]";
    json << "}";
    return alloc_slice(json.str());
}


