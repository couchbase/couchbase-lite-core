//
//  DBEndpoint.cc
//  LiteCore
//
//  Created by Jens Alfke on 8/19/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "DBEndpoint.hh"
#include "RemoteEndpoint.hh"
#include "c4Document+Fleece.h"
#include "c4Replicator.h"
#include "Stopwatch.hh"
#include <algorithm>
#include <thread>


void DbEndpoint::prepare(bool isSource, bool mustExist, slice docIDProperty, const Endpoint *other) {
    Endpoint::prepare(isSource, mustExist, docIDProperty, other);
    _otherEndpoint = const_cast<Endpoint*>(other);
    C4DatabaseConfig config = {kC4DB_Bundled | kC4DB_SharedKeys | kC4DB_NonObservable};
    if (isSource) {
        if (!other->isDatabase())    // need write permission if replicating, even for push
            config.flags |= kC4DB_ReadOnly;
    } else {
        if (!mustExist)
            config.flags |= kC4DB_Create;
    }
    C4Error err;
    _db = c4db_open(c4str(_spec), &config, &err);
    if (!_db)
        fail(format("Couldn't open database %s", _spec.c_str()), err);

    // Only used for writing JSON:
    auto sk = c4db_getFLSharedKeys(_db);
    _encoder.setSharedKeys(sk);
    if (docIDProperty) {
        _docIDPath.reset(new KeyPath(docIDProperty, sk, nullptr));
        if (!*_docIDPath)
            fail("Invalid key-path");
    }
}


void DbEndpoint::enterTransaction() {
    if (!_inTransaction) {
        C4Error err;
        if (!c4db_beginTransaction(_db, &err))
            fail("starting transaction", err);
        _inTransaction = true;
    }
}


// As source:
void DbEndpoint::copyTo(Endpoint *dst, uint64_t limit) {
    // Special cases: database to database (local or remote)
    auto dstDB = dynamic_cast<DbEndpoint*>(dst);
    if (dstDB)
        return pushToLocal(*dstDB);
    auto remoteDB = dynamic_cast<RemoteEndpoint*>(dst);
    if (remoteDB)
        return replicateWith(*remoteDB, kC4OneShot, kC4Disabled);
    // Normal case, copying docs (to JSON, presumably):
    exportTo(dst, limit);
}


void DbEndpoint::exportTo(Endpoint *dst, uint64_t limit) {
    if (gVerbose)
        cout << "Exporting documents...\n";
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


// As destination of JSON file(s):
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

    enterTransaction();

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
        enterTransaction();
    }
}


void DbEndpoint::finish() {
    commit();
    C4Error err;
    if (!c4db_close(_db, &err))
        errorOccurred("closing database", err);
}


void DbEndpoint::commit() {
    if (_inTransaction) {
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
}


#pragma mark - REPLICATION:


void DbEndpoint::replicateWith(RemoteEndpoint &src, C4ReplicatorMode push, C4ReplicatorMode pull) {
    if (gVerbose) {
        if (push >= kC4OneShot)
            cout << "Pushing to remote database...\n";
        if (pull >= kC4OneShot)
            cout << "Pulling from remote database...\n";
    }
    C4ReplicatorParameters params = replicatorParameters(push, pull);
    C4Error err;
    replicate(c4repl_new(_db, src.address(), src.databaseName(), nullptr, params, &err), err);
}


void DbEndpoint::pushToLocal(DbEndpoint &dst) {
    if (gVerbose)
        cout << "Pushing to local database...\n";
    C4ReplicatorParameters params = replicatorParameters(kC4OneShot, kC4Disabled);
    C4Error err;
    replicate(c4repl_new(_db, {}, nullslice, dst._db, params, &err), err);
}


C4ReplicatorParameters DbEndpoint::replicatorParameters(C4ReplicatorMode push, C4ReplicatorMode pull) {
    C4ReplicatorParameters params = {};
    params.push = push;
    params.pull = pull;
    params.callbackContext = this;

    params.onStatusChanged = [](C4Replicator *replicator,
                                C4ReplicatorStatus status,
                                void *context)
    {
        ((DbEndpoint*)context)->onStateChanged(status);
    };

    params.onDocumentError = [](C4Replicator *repl,
                                bool pushing,
                                C4String docID,
                                C4Error error,
                                bool transient,
                                void *context)
    {
        ((DbEndpoint*)context)->onDocError(pushing, docID, error, transient);
    };
    return params;
}


void DbEndpoint::replicate(C4Replicator *repl, C4Error &err) {
    if (!repl)
        errorOccurred("starting replication", err);

    c4::ref<C4Replicator> replicator = repl;
    C4ReplicatorStatus status;
    while ((status = c4repl_getStatus(replicator)).level != kC4Stopped)
        this_thread::sleep_for(chrono::milliseconds(100));
    startLine();
}


void DbEndpoint::onStateChanged(C4ReplicatorStatus status) {
    if (gVerbose) {
        cout << "\r" << kC4ReplicatorActivityLevelNames[status.level] << " ... ";
        _needNewline = true;
        if (status.progress.documentCount > 0)
            cout << status.progress.documentCount << " documents ";
        if (status.progress.unitsTotal > 0) {
            double progress = status.progress.unitsTotal ? (status.progress.unitsCompleted / (double)status.progress.unitsTotal) : 0.0;
            printf("(%.0f%%)", round(progress * 100.0));
        }
        if (status.level == kC4Stopped)
            startLine();
        cout.flush();
    }

    if (status.error.code != 0) {
        startLine();
        char message[200];
        c4error_getMessageC(status.error, message, sizeof(message));
        C4Log("** Replicator error: %s (%d,%d)",
              message, status.error.domain, status.error.code);
    }

    setDocCount(status.progress.documentCount);
    _otherEndpoint->setDocCount(status.progress.documentCount);
}


void DbEndpoint::onDocError(bool pushing,
                            C4String docID,
                            C4Error error,
                            bool transient)
{
    if (error.code == 0) {
       // _otherEndpoint->logDocument(docID);
    } else {
        startLine();
        char message[200];
        c4error_getMessageC(error, message, sizeof(message));
        C4Log("** Error %s doc \"%.*s\": %s (%d,%d)",
              (pushing ? "pushing" : "pulling"),
              (int)docID.size, docID.buf,
              message, error.domain, error.code)
    }
}


void DbEndpoint::startLine() {
    if (_needNewline) {
        cout << "\n";
        _needNewline = false;
    }
}
