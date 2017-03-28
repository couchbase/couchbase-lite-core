package com.couchbase.litecore;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.fail;

public class C4QueryTest extends BaseTest {
    @Test
    public void testDatabaseErrorMessages() {
        try {
            C4Query query = new C4Query(this.db, "[\"=\"]");
            fail();
        } catch (LiteCoreException e) {
            assertEquals(C4ErrorDomain.LiteCoreDomain, e.domain);
            assertEquals(LiteCoreError.kC4ErrorInvalidQuery, e.code);
        }
    }
}
