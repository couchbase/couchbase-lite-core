package com.couchbase.litecore;

import com.couchbase.lite.Log;
import com.couchbase.litecore.fleece.FLValue;

import java.util.ArrayList;
import java.util.List;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

public class C4QueryBaseTest extends BaseTest{
    //-------------------------------------------------------------------------
    // protected variables
    //-------------------------------------------------------------------------
    protected C4Query query;

    //-------------------------------------------------------------------------
    // protected methods
    //-------------------------------------------------------------------------

    protected String getColumn(byte[] customColumns, int i) {
        assertNotNull(customColumns);
        List<Object> colsArray = FLValue.fromData(customColumns).asArray();
        assertTrue(colsArray.size() >= i + 1);
        return (String) colsArray.get(i);
    }

    protected C4Query compile(String whereExpr) throws LiteCoreException {
        return compile(whereExpr, null);
    }

    protected C4Query compile(String whereExpr, String sortExpr) throws LiteCoreException {
        Log.e(LOG_TAG, "whereExpr -> " + whereExpr);
        Log.e(LOG_TAG, "sortExpr -> " + sortExpr);
        String queryString = whereExpr;
        if (sortExpr != null && sortExpr.length() > 0)
            queryString = "[\"SELECT\", {\"WHERE\": " + whereExpr + ", \"ORDER_BY\": " + sortExpr + "}]";
        Log.i(LOG_TAG, "Query = %s", queryString);
        if (query != null) {
            query.free();
            query = null;
        }
        query = new C4Query(db, queryString);
        assertNotNull(query);
        Log.e(LOG_TAG, "expression -> " + query.explain());
        return query;
    }

    protected List<String> run() throws LiteCoreException {
        return run(0);
    }

    protected List<String> run(long skip) throws LiteCoreException {
        return run(skip, Long.MAX_VALUE);
    }

    protected List<String> run(long skip, long limit) throws LiteCoreException {
        return run(skip, limit, null);
    }

    protected List<String> run(long skip, long limit, String bindings) throws LiteCoreException {
        List<String> docIDs = new ArrayList<>();
        C4QueryOptions opts = new C4QueryOptions();
        opts.setSkip(skip);
        opts.setLimit(limit);
        C4QueryEnumerator e = query.run(opts, bindings);
        assertNotNull(e);
        while (e.next()) {
            docIDs.add(e.getDocID());
        }
        e.free();
        return docIDs;
    }
}
