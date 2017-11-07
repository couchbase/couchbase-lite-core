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
    // Constructors
    //-------------------------------------------------------------------------

    C4Query(long db, String expression) throws LiteCoreException {
        handle = init(db, expression);
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    public void free() {
        if (handle != 0L) {
            free(handle);
            handle = 0L;
        }
    }

    public String explain() {
        return explain(handle);
    }

    public int columnCount() {
        return columnCount(handle);
    }

    public C4QueryEnumerator run(C4QueryOptions options, String encodedParameters)
            throws LiteCoreException {
        return new C4QueryEnumerator(
                run(handle, options.rankFullText, encodedParameters));
    }

    public byte[] getFullTextMatched(long fullTextID) throws LiteCoreException {
        return getFullTextMatched(handle, fullTextID);
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

    //////// DATABASE QUERIES:

    /**
     * @param db
     * @param expression
     * @return C4Query*
     * @throws LiteCoreException
     */
    static native long init(long db, String expression) throws LiteCoreException;

    /**
     * Free C4Query* instance
     *
     * @param c4query (C4Query*)
     */
    static native void free(long c4query);

    /**
     * @param c4query (C4Query*)
     * @return C4StringResult
     */
    static native String explain(long c4query);

    /**
     * Returns the number of columns (the values specified in the WHAT clause) in each row.
     *
     * @param c4query (C4Query*)
     * @return the number of columns
     */
    static native int columnCount(long c4query);

    //////// RUNNING QUERIES:

    /**
     * @param c4query
     * @param rankFullText
     * @param encodedParameters
     * @return C4QueryEnumerator*
     * @throws LiteCoreException
     */
    static native long run(long c4query,
                           boolean rankFullText,
                           String encodedParameters)
            throws LiteCoreException;

    /**
     * Given a docID and sequence number from the enumerator, returns the text that was emitted
     * during indexing.
     */
    static native byte[] getFullTextMatched(long c4query, long fullTextID) throws LiteCoreException;

    //////// INDEXES:

    // - Creates a database index, to speed up subsequent queries.

    static native boolean createIndex(long db,
                                      String name,
                                      String expressionsJSON,
                                      int indexType,
                                      String language,
                                      boolean ignoreDiacritics)
            throws LiteCoreException;

    static native void deleteIndex(long db, String name) throws LiteCoreException;

    /**
     * Gets a fleece encoded array of indexes in the given database
     * that were created by `c4db_createIndex`
     *
     * @param db
     * @return pointer to FLValue
     * @throws LiteCoreException
     */
    static native long getIndexes(long db) throws LiteCoreException;
}
