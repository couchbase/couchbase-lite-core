package com.couchbase.litecore;

import com.couchbase.litecore.fleece.FLEncoder;
import com.couchbase.litecore.fleece.FLValue;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;

import java.util.HashMap;
import java.util.Map;

import static com.couchbase.litecore.fleece.FLConstants.FLValueType.kFLData;
import static com.couchbase.litecore.fleece.FLConstants.FLValueType.kFLDict;
import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

public class C4FleeceTest extends C4BaseTest {
    private static final String TAG = C4FleeceTest.class.getSimpleName();

    @Before
    public void setUp() throws Exception {
        super.setUp();
    }

    @After
    public void tearDown() throws Exception {
        super.tearDown();
    }

    @Test
    public void testEncodeBytes() throws LiteCoreException {
        byte[] input = "Hello World!".getBytes();

        FLEncoder enc = new FLEncoder();
        enc.writeData(input);
        byte[] optionsFleece = enc.finish();
        assertNotNull(optionsFleece);

        FLValue value = FLValue.fromData(optionsFleece);
        assertNotNull(value);
        assertEquals(kFLData, value.getType());
        byte[] output = value.asData();
        assertNotNull(output);
        assertArrayEquals(input, output);
    }

    @Test
    public void testEncodeMapWithBytes() throws LiteCoreException {
        byte[] input = "Hello World!".getBytes();
        Map<String, Object> map = new HashMap<>();
        map.put("bytes", input);

        FLEncoder enc = new FLEncoder();
        enc.write(map);
        byte[] optionsFleece = enc.finish();
        assertNotNull(optionsFleece);

        FLValue value = FLValue.fromData(optionsFleece);
        assertNotNull(value);
        assertEquals(kFLDict, value.getType());
        Map<String, Object> map2 = value.asDict();
        assertNotNull(map2);
        assertTrue(map2.containsKey("bytes"));
        byte[] output = (byte[]) map2.get("bytes");
        assertNotNull(output);
        assertArrayEquals(input, output);
    }
}
