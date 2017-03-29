package com.couchbase.litecore;

import org.junit.Test;

import java.io.File;
import java.util.Arrays;

import static org.junit.Assert.assertEquals;


/**
 * Created by hideki on 3/28/17.
 */

public class C4NestedQueryTest  extends C4QueryBaseTest {
    static final String LOG_TAG = C4QueryTest.class.getSimpleName();

    C4Query query = null;

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    @Override
    public void setUp() throws Exception {
        super.setUp();
        Utils.copyAssets("nested.json", context.getFilesDir());
        importJSONLines(new File(context.getFilesDir(), "nested.json"));
    }

    @Override
    public void tearDown() throws Exception {
        if (query != null) {
            query.free();
            query = null;
        }
        super.tearDown();
    }
    // - DB Query ANY nested
    @Test
    public void testDBQueryANYNested() throws LiteCoreException, Exception {
        compile(json5("['ANY', 'Shape', ['.', 'shapes'], ['=', ['?', 'Shape', 'color'], 'red']]"));
        assertEquals(Arrays.asList("0000001", "0000003") , run());
    }
}
