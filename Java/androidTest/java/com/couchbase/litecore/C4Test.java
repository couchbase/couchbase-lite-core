package com.couchbase.litecore;


import org.junit.Test;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

public class C4Test extends C4BaseTest {
    @Test
    public void testGetBuildInfo() {
        String res = C4.getBuildInfo();
        assertNotNull(res);
        assertTrue(res.length() > 0);
    }

    @Test
    public void testGetVersion() {
        String res = C4.getVersion();
        assertNotNull(res);
        assertTrue(res.length() > 0);
    }
}

