//
// SQLiteKeyStore+Indexes.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//


#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "SQLite_Internal.hh"
#include "Query.hh"
#include "QueryParser.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "Stopwatch.hh"
#include "Array.hh"

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    /*
     - A value index is a SQL index named 'NAME'.
     - A FTS index is a SQL virtual table named 'kv_default::NAME'
     - An array index has two parts:
         * A SQL table named `kv_default:unnest:PATH`, where PATH is the property path
         * An index on that table named `NAME`
     - A predictive index has two parts:
         * A SQL table named `kv_default:prediction:DIGEST`, where DIGEST is a unique digest
            of the prediction function name and the parameter dictionary
         * An index on that table named `NAME`

     Index table:
        - name (string primary key)
        - type (integer)
        - expression (JSON)
        - table name (string)
     The SQL index always is always named `name`.
     */


    bool SQLiteKeyStore::createIndex(const IndexSpec &spec) {
        spec.validateName();

        Stopwatch st;
        ExclusiveTransaction t(db());
        bool created;
        switch (spec.type) {
            case IndexSpec::kValue:      created = createValueIndex(spec); break;
            case IndexSpec::kFullText:   created = createFTSIndex(spec); break;
            case IndexSpec::kArray:      created = createArrayIndex(spec); break;
#ifdef COUCHBASE_ENTERPRISE
            case IndexSpec::kPredictive: created = createPredictiveIndex(spec); break;
#endif
            default:                     error::_throw(error::Unimplemented);
        }

        if (created) {
            t.commit();
            double time = st.elapsed();
            QueryLog.log((time < 3.0 ? LogLevel::Info : LogLevel::Warning),
                         "Created index '%s' in %.3f sec", spec.name.c_str(), time);
        }
        return created;
    }


    // Actually creates the index (called by the createXXXIndex methods)
    bool SQLiteKeyStore::createIndex(const IndexSpec &spec,
                                     const string &sourceTableName,
                                     Array::iterator &expressions)
    {
        Assert(spec.type != IndexSpec::kFullText);
        QueryParser qp(db(), "", sourceTableName);
        qp.writeCreateIndex(spec.name,
                            sourceTableName,
                            expressions,
                            spec.where(),
                            (spec.type != IndexSpec::kValue));
        string sql = qp.SQL();
        return db().createIndex(spec, this, sourceTableName, sql);
    }


    void SQLiteKeyStore::deleteIndex(slice name)  {
        ExclusiveTransaction t(db());
        auto spec = db().getIndex(name);
        if (spec) {
            db().deleteIndex(*spec);
            t.commit();
        } else {
            t.abort();
        }
    }


    // Creates the special by-sequence index
    void SQLiteKeyStore::createSequenceIndex() {
        if (!_createdSeqIndex) {
            Assert(_capabilities.sequences);
            db().execWithLock(subst(
                            "CREATE UNIQUE INDEX IF NOT EXISTS \"kv_@_seqs\" ON kv_@ (sequence)"));
            _createdSeqIndex = true;
        }
    }


    // Creates indexes on flags
    void SQLiteKeyStore::_createFlagsIndex(const char *indexName, DocumentFlags flag, bool &created) {
        if (!created) {
            db().execWithLock(CONCAT("CREATE INDEX IF NOT EXISTS \"" << tableName() << "_" << indexName <<
                                 "\" ON " << quotedTableName() << " (flags)"
                                 " WHERE (flags & " << int(flag) << ") != 0"));
            created = true;
        }
    }

    void SQLiteKeyStore::createConflictsIndex() {
        _createFlagsIndex("conflicts", DocumentFlags::kConflicted, _createdConflictsIndex);
    }

    void SQLiteKeyStore::createBlobsIndex() {
        _createFlagsIndex("blobs", DocumentFlags::kHasAttachments, _createdBlobsIndex);
    }


    vector<IndexSpec> SQLiteKeyStore::getIndexes() const {
        vector<IndexSpec> result;
        for (auto &spec : db().getIndexes(this))
            result.push_back(std::move(spec));
        return result;
    }


#pragma mark - VALUE INDEX:


    bool SQLiteKeyStore::createValueIndex(const IndexSpec &spec) {
        Array::iterator expressions(spec.what());
        return createIndex(spec, tableName(), expressions);
    }

}
