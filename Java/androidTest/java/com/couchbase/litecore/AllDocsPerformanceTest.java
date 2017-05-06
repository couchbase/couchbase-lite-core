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

import com.couchbase.litecore.utils.StopWatch;

import org.junit.Before;
import org.junit.Test;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;

/**
 * Ported from c4AllDocsPerformanceTest.cc
 */
public class AllDocsPerformanceTest extends BaseTest {
    static final String TAG = "AllDocsPerformanceTest";

    static final int kSizeOfDocument = 1000;
    static final int kNumDocuments = 1000; // 100000

    @Before
    public void setUp() throws Exception {

        super.setUp();

        char[] chars = new char[kSizeOfDocument];
        Arrays.fill(chars, 'a');
        final String content = new String(chars);

        boolean commit = false;
        db.beginTransaction();
        try {
            Random random = new Random();
            for (int i = 0; i < kNumDocuments; i++) {
                String docID = String.format("doc-%08x-%08x-%08x-%04x", random.nextLong(), random.nextLong(), random.nextLong(), i);
                String json = String.format("{\"content\":\"%s\"}", content);
                List<String> list = new ArrayList<String>();
                if (isRevTrees())
                    list.add("1-deadbeefcafebabe80081e50");
                else
                    list.add("1@deadbeefcafebabe80081e50");
                String[] history = list.toArray(new String[list.size()]);
                Document doc = db.put(docID, json.getBytes(), true, false, history, 0, true, 0);
                assertNotNull(doc);
                doc.free();
            }
            commit = true;
        } finally {
            db.endTransaction(commit);
        }

        assertEquals(kNumDocuments, db.getDocumentCount());
    }

    // - AllDocsPerformance
    @Test
    public void testAllDocsPerformance() throws LiteCoreException {
        StopWatch st = new StopWatch();
        st.start();

        // No start or end ID:
        int iteratorFlags = IteratorFlags.kDefault;
        iteratorFlags &= ~IteratorFlags.kIncludeBodies;
        DocumentIterator itr = db.iterator(null, null, 0, iteratorFlags);
        Document doc;
        int i = 0;
        while ((doc = itr.nextDocument()) != null) {
            try {
                i++;
            } finally {
                doc.free();
            }
        }
        assertEquals(kNumDocuments, i);

        double elapsed = st.getElapsedTimeMillis();
        Log.i(TAG, String.format("Enumerating %d docs took %.3f ms (%.3f ms/doc)", i, elapsed, elapsed / i));
    }
}
