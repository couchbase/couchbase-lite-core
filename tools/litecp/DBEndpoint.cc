//
//  DBEndpoint.cc
//  LiteCore
//
//  Created by Jens Alfke on 8/19/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "DBEndpoint.hh"
#include "c4Document+Fleece.h"
#include "Stopwatch.hh"


void DbEndpoint::prepare(bool readOnly, bool mustExist, slice docIDProperty) {
    Endpoint::prepare(readOnly, mustExist, docIDProperty);
    C4DatabaseConfig config = {kC4DB_Bundled | kC4DB_SharedKeys | kC4DB_NonObservable};
    if (readOnly)
        config.flags |= kC4DB_ReadOnly;
    else if (!mustExist)
        config.flags |= kC4DB_Create;
    C4Error err;
    _db = c4db_open(c4str(_spec), &config, &err);
    if (!_db)
        fail(format("Couldn't open database %s", _spec.c_str()), err);
    if (!c4db_beginTransaction(_db, &err))
        fail("starting transaction", err);

    auto sk = c4db_getFLSharedKeys(_db);
    _encoder.setSharedKeys(sk);
    if (docIDProperty) {
        _docIDPath.reset(new KeyPath(docIDProperty, sk, nullptr));
        if (!*_docIDPath)
            fail("Invalid key-path");
    }
}


// As source:
void DbEndpoint::copyTo(Endpoint *dst, uint64_t limit) {
    // Special case: database to database
    auto dstDB = dynamic_cast<DbEndpoint*>(dst);
    if (dstDB)
        return pushTo(dstDB);

    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    C4Error err;
    c4::ref<C4DocEnumerator> e = c4db_enumerateAllDocs(_db, nullslice, nullslice, &options, &err);
    if (!e)
        fail("enumerating source db", err);
    uint64_t line;
    for (line = 0; line < limit; ++line) {
        c4::ref<C4Document> doc = c4enum_nextDocument(e, &err);
        if (!doc)
            break;
        alloc_slice json = c4doc_bodyAsJSON(doc, false, &err);
        if (!json) {
            errorOccurred("reading document body", err);
            continue;
        }
        dst->writeJSON(doc->docID, json);
    }
    if (err.code)
        errorOccurred("enumerating source db", err);
    else if (line == limit)
        cout << "Stopped after " << limit << " documents.\n";
}


// As destination:
void DbEndpoint::writeJSON(slice docID, slice json) {
    _encoder.reset();
    if (!_encoder.convertJSON(json)) {
        errorOccurred(format("Couldn't parse JSON: %.*s", SPLAT(json)));
        return;
    }
    alloc_slice body = _encoder.finish();

    // Get the JSON's docIDProperty to use as the document ID:
    alloc_slice docIDBuf;
    if (!docID && _docIDProperty)
        docID = docIDBuf = docIDFromFleece(body, json);

    C4DocPutRequest put { };
    put.docID = docID;
    put.body = body;
    put.save = true;
    C4Error err;
    c4::ref<C4Document> doc = c4doc_put(_db, &put, nullptr, &err);
    if (doc) {
        docID = slice(doc->docID);
    } else {
        if (docID)
            errorOccurred(format("saving document \"%.*s\"", SPLAT(put.docID)), err);
        else
            errorOccurred("saving document", err);
    }

    logDocument(docID);

    if (++_transactionSize >= kMaxTransactionSize) {
        commit();
        if (!c4db_beginTransaction(_db, &err))
            fail("starting transaction", err);
    }
}


void DbEndpoint::finish() {
    commit();
    C4Error err;
    if (!c4db_close(_db, &err))
        errorOccurred("closing database", err);
}


void DbEndpoint::commit() {
    if (gVerbose > 1) {
        cout << "[Committing ... ";
        cout.flush();
    }
    Stopwatch st;
    C4Error err;
    if (!c4db_endTransaction(_db, true, &err))
        fail("committing transaction", err);
    if (gVerbose > 1) {
        double time = st.elapsed();
        cout << time << " sec for " << _transactionSize << " docs]\n";
    }
    _transactionSize = 0;
}


void DbEndpoint::pushTo(DbEndpoint *) {
    fail("Sorry, db-to-db replication is not implemented yet");
}
