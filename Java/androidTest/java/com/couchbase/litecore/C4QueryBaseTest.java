/**
 * Copyright (c) 2017 Couchbase, Inc. All rights reserved.
 * <p>
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of the License at
 * <p>
 * http://www.apache.org/licenses/LICENSE-2.0
 * <p>
 * Unless required by applicable law or agreed to in writing, software distributed under the
 * License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */
package com.couchbase.litecore;

import android.util.Log;

import java.util.ArrayList;
import java.util.List;

import static org.junit.Assert.assertNotNull;

public class C4QueryBaseTest extends C4BaseTest {
    public static final String LOG_TAG = C4QueryBaseTest.class.getSimpleName();

    //-------------------------------------------------------------------------
    // protected variables
    //-------------------------------------------------------------------------
    protected C4Query query;

    //-------------------------------------------------------------------------
    // protected methods
    //-------------------------------------------------------------------------

    protected C4Query compileSelect(String queryStr) throws LiteCoreException {
        Log.i(LOG_TAG, "Query -> " + queryStr);

        if (query != null) {
            query.free();
            query = null;
        }
        query = db.createQuery(queryStr);
        assertNotNull(query);

        Log.i(LOG_TAG, "query.explain() -> " + query.explain());

        return query;
    }

    protected C4Query compile(String whereExpr) throws LiteCoreException {
        return compile(whereExpr, null);
    }

    protected C4Query compile(String whereExpr, String sortExpr) throws LiteCoreException {
        return compile(whereExpr, sortExpr, false);
    }

    protected C4Query compile(String whereExpr, String sortExpr, boolean addOffsetLimit) throws LiteCoreException {
        Log.i(LOG_TAG, "whereExpr -> " + whereExpr + ", sortExpr -> " + sortExpr + ", addOffsetLimit -> " + addOffsetLimit);

        StringBuffer json = new StringBuffer();
        json.append("[\"SELECT\", {\"WHERE\": ");
        json.append(whereExpr);
        if (sortExpr != null && sortExpr.length() > 0) {
            json.append(", \"ORDER_BY\": ");
            json.append(sortExpr);
        }
        if (addOffsetLimit) {
            json.append(", \"OFFSET\": [\"$offset\"], \"LIMIT\":  [\"$limit\"]");
        }
        json.append("}]");

        Log.i(LOG_TAG, "Query = " + json.toString());

        if (query != null) {
            query.free();
            query = null;
        }
        query = db.createQuery(json.toString());
        assertNotNull(query);

        Log.i(LOG_TAG, "query.explain() -> " + query.explain());

        return query;
    }

    protected List<String> run() throws LiteCoreException {
        return run(null);
    }

    protected List<String> run(String bindings) throws LiteCoreException {
        List<String> docIDs = new ArrayList<>();
        C4QueryOptions opts = new C4QueryOptions();
        C4QueryEnumerator e = query.run(opts, bindings);
        assertNotNull(e);
        while (e.next()) {
            docIDs.add(e.getDocID());
        }
        e.free();
        return docIDs;
    }
}
