package com.couchbase.litecore;

import com.couchbase.lite.*;

import org.junit.Test;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import static com.couchbase.litecore.Constants.C4IndexType.kC4FullTextIndex;
import static com.couchbase.litecore.Constants.C4IndexType.kC4ValueIndex;
import static com.couchbase.litecore.Constants.C4RevisionFlags.kRevDeleted;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.fail;

public class C4QueryTest extends BaseTest {
    static final String LOG_TAG = C4QueryTest.class.getSimpleName();

    C4Query query = null;

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    @Override
    public void setUp() throws Exception {
        super.setUp();
        Utils.copyAssets("names_100.json", context.getFilesDir());
        importJSONLines(new File(context.getFilesDir(), "names_100.json"));
    }

    @Override
    public void tearDown() throws Exception {
        if (query != null) {
            query.free();
            query = null;
        }
        super.tearDown();
    }

    //-------------------------------------------------------------------------
    // tests
    //-------------------------------------------------------------------------

    // -- Query parser error messages
    @Test
    public void testDatabaseErrorMessages() {
        try {
            new C4Query(db, "[\"=\"]");
            fail();
        } catch (LiteCoreException e) {
            assertEquals(C4ErrorDomain.LiteCoreDomain, e.domain);
            assertEquals(LiteCoreError.kC4ErrorInvalidQuery, e.code);
        }
    }

    // - DB Query
    @Test
    public void testDBQuery() throws LiteCoreException {
        compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"));
        assertEquals(Arrays.asList("0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"), run());
        assertEquals(Arrays.asList("0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"), run(1, 8));
        assertEquals(Arrays.asList("0000015", "0000036", "0000043", "0000053"), run(1, 4));

        compile(json5("['AND', ['=', ['array_count()', ['.', 'contact', 'phone']], 2],['=', ['.', 'gender'], 'male']]"));
        assertEquals(Arrays.asList("0000002", "0000014", "0000017", "0000027", "0000031", "0000033", "0000038", "0000039", "0000045", "0000047",
                "0000049", "0000056", "0000063", "0000065", "0000075", "0000082", "0000089", "0000094", "0000097"), run());

        // MISSING means no value is present (at that array index or dict key)
        compile(json5("['IS', ['.', 'contact', 'phone', [0]], ['MISSING']]"));
        assertEquals(Arrays.asList("0000004", "0000006", "0000008", "0000015"), run(0, 4));

        // ...wherease null is a JSON null value
        compile(json5("['IS', ['.', 'contact', 'phone', [0]], null]"));
        assertEquals(Arrays.asList(), run(0, 4));
    }

    // - DB Query sorted
    @Test
    public void testDBQuerySorted() throws LiteCoreException {
        compile(json5("['=', ['.', 'contact', 'address', 'state'], 'CA']"),
                json5("[['.', 'name', 'last']]"));
        assertEquals(Arrays.asList("0000015", "0000036", "0000072", "0000043", "0000001", "0000064", "0000073", "0000053"), run());
    }

    // - DB Query bindings
    @Test
    public void testDBQueryBindings() throws LiteCoreException {
        compile(json5("['=', ['.', 'contact', 'address', 'state'], ['$', 1]]"));
        assertEquals(Arrays.asList("0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"), run(0, Long.MAX_VALUE, "{\"1\": \"CA\"}"));

        compile(json5("['=', ['.', 'contact', 'address', 'state'], ['$', 'state']]"));
        assertEquals(Arrays.asList("0000001", "0000015", "0000036", "0000043", "0000053", "0000064", "0000072", "0000073"), run(0, Long.MAX_VALUE, "{\"state\": \"CA\"}"));
    }

    // - DB Query ANY
    @Test
    public void testDBQueryANY() throws LiteCoreException {
        compile(json5("['ANY', 'like', ['.', 'likes'], ['=', ['?', 'like'], 'climbing']]"));
        assertEquals(Arrays.asList("0000017", "0000021", "0000023", "0000045", "0000060"), run());

        // This EVERY query has lots of results because every empty `likes` array matches it
        compile(json5("['EVERY', 'like', ['.', 'likes'], ['=', ['?', 'like'], 'taxes']]"));
        List<String> result = run();
        assertEquals(42, result.size());
        assertEquals("0000007", result.get(0));

        // Changing the op to ANY AND EVERY returns no results
        compile(json5("['ANY AND EVERY', 'like', ['.', 'likes'], ['=', ['?', 'like'], 'taxes']]"));
        assertEquals(Arrays.asList(), run());

        // Look for people where every like contains an L:
        compile(json5("['ANY AND EVERY', 'like', ['.', 'likes'], ['LIKE', ['?', 'like'], '%l%']]"));
        assertEquals(Arrays.asList("0000017", "0000027", "0000060", "0000068"), run());
    }

    // - DB Query expression index
    @Test
    public void testDBQueryExpressionIndex() throws LiteCoreException {
        db.createIndex(json5("[['length()', ['.name.first']]]"), kC4ValueIndex, null, true);
        compile(json5("['=', ['length()', ['.name.first']], 9]"));
        assertEquals(Arrays.asList("0000015", "0000099"), run(0, Long.MAX_VALUE));
    }

    // - Delete indexed doc
    @Test
    public void testDeleteIndexedDoc() throws LiteCoreException {
        // Create the same index as the above test:
        db.createIndex(json5("[['length()', ['.name.first']]]"), kC4ValueIndex, null, true);

        // Delete doc "0000015":
        {
            boolean commit = false;
            db.beginTransaction();
            try {
                Document doc = db.getDocument("0000015", true);
                assertNotNull(doc);
                String[] history = {doc.getRevID()};
                Document updatedDoc = db.put(doc.getDocID(), (byte[]) null, null, false, false, history, kRevDeleted, true, 0);
                assertNotNull(updatedDoc);
                doc.free();
                updatedDoc.free();
                commit = true;
            }finally {
                db.endTransaction(commit);
            }

        }

        // Now run a query that would have returned the deleted doc, if it weren't deleted:
        compile(json5("['=', ['length()', ['.name.first']], 9]"));
        assertEquals(Arrays.asList("0000099"), run(0, Long.MAX_VALUE));
    }


    // - Full-text query
    @Test
    public void testFullTextQuery() throws LiteCoreException {
        db.createIndex("[[\".contact.address.street\"]]", kC4FullTextIndex, null, true);
        compile(json5("['MATCH', ['.', 'contact', 'address', 'street'], 'Hwy']"));
        assertEquals(Arrays.asList("0000013", "0000015", "0000043", "0000044", "0000052"), run(0, Long.MAX_VALUE));
    }

    // - DB Query WHAT
    // - DB Query Aggregate
    // - DB Query Grouped
    // - DB Query ANY nested

    //-------------------------------------------------------------------------
    // private methods
    //-------------------------------------------------------------------------

    private C4Query compile(String whereExpr) throws LiteCoreException {
        return compile(whereExpr, null);
    }

    private C4Query compile(String whereExpr, String sortExpr) throws LiteCoreException {
        Log.e(LOG_TAG, "whereExpr -> "+whereExpr);
        Log.e(LOG_TAG, "sortExpr -> "+sortExpr);
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
        Log.e(LOG_TAG, "expression -> "+query.explain());
        return query;
    }

    private List<String> run() throws LiteCoreException {
        return run(0);
    }

    private List<String> run(long skip) throws LiteCoreException {
        return run(skip, Long.MAX_VALUE);
    }

    private List<String> run(long skip, long limit) throws LiteCoreException {
        return run(skip, limit, null);
    }

    private List<String> run(long skip, long limit, String bindings) throws LiteCoreException {
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
