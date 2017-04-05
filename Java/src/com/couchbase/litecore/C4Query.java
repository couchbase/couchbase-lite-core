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

public class C4Query {
    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    private long handle = 0L; // hold pointer to C4Query

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public C4Query(Database db, String expression) throws LiteCoreException {
        handle = init(db._handle, expression);
    }

    public void free() {
        if (handle != 0L) {
            free(handle);
            handle = 0L;
        }
    }

    public String explain() {
        return explain(handle);
    }

    public C4QueryEnumerator run(C4QueryOptions options, String encodedParameters)
            throws LiteCoreException {
        return new C4QueryEnumerator(
                run(handle, options.skip, options.limit, options.rankFullText, encodedParameters));
    }

    public byte[] getFullTextMatched(String docID, long seq) throws LiteCoreException {
        return getFullTextMatched(handle, docID, seq);
    }

    //-------------------------------------------------------------------------
    // protected methods
    //-------------------------------------------------------------------------
    @Override
    protected void finalize() throws Throwable {
        free();
        super.finalize();
    }

    //-------------------------------------------------------------------------
     // native methods
    //-------------------------------------------------------------------------

    /**
     * @param db
     * @param expression
     * @return C4Query*
     * @throws LiteCoreException
     */
    private native static long init(long db, String expression) throws LiteCoreException;

    /**
     * Free C4Query* instance
     *
     * @param c4query (C4Query*)
     */
    private static native void free(long c4query);

    /**
     * @param c4query (C4Query*)
     * @return C4StringResult
     */
    private static native String explain(long c4query);

    /**
     * @param c4query
     * @param skip
     * @param limit
     * @param rankFullText
     * @param encodedParameters
     * @return C4QueryEnumerator*
     * @throws LiteCoreException
     */
    private static native long run(long c4query,
                                   long skip,
                                   long limit,
                                   boolean rankFullText,
                                   String encodedParameters)
            throws LiteCoreException;

    private static native byte[] getFullTextMatched(long c4query, String docID, long seq)
            throws LiteCoreException;
}
