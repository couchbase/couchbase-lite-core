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

import com.couchbase.litecore.fleece.FLArrayIterator;

public class C4QueryEnumerator {
    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------

    private long handle = 0L; // hold pointer to C4QueryEnumerator

    //-------------------------------------------------------------------------
    // Constructor
    //-------------------------------------------------------------------------
    /* package */C4QueryEnumerator(long handle) {
        this.handle = handle;
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    public byte[] getFullTextMatched() throws LiteCoreException {
        if (handle != 0)
            return getFullTextMatched(handle);
        else
            return null;
    }

    public boolean next() throws LiteCoreException {
        boolean ok = next(handle);
        // NOTE: Please keep folowing line of code for a while.
        //if (!ok)
        //    handle = 0;
        return ok;
    }

    public long getRowCount() throws LiteCoreException {
        return getRowCount(handle);
    }

    public boolean seek(long rowIndex) throws LiteCoreException {
        return seek(handle, rowIndex);
    }

    public C4QueryEnumerator refresh() throws LiteCoreException {
        // handle is closed or reached end.
        if (handle == 0)
            return null;
        long newHandle = refresh(handle);
        if (newHandle == 0)
            return null;
        return new C4QueryEnumerator(newHandle);
    }

    public void close() {
        if (handle != 0)
            close(handle);
    }

    public boolean isClosed() {
        return handle == 0;
    }

    public void free() {
        if (handle != 0) {
            free(handle);
            handle = 0;
        }
    }

    // -- Accessor methods to C4QueryEnumerator --

    public String getDocID() {
        return getDocID(handle);
    }

    public long getDocSequence() {
        return getDocSequence(handle);
    }

    public String getRevID() {
        return getRevID(handle);
    }

    public long getDocFlags() {
        return getDocFlags(handle);
    }

    // NOTE: FLArrayIterator is member variable of C4QueryEnumerator. Not necessary to release.
    public FLArrayIterator getColumns() {
        return new FLArrayIterator(getColumns(handle));
    }

    public long getFullTextTermCount() {
        return getFullTextTermCount(handle);
    }

    public long getFullTextTermIndex(long pos) {
        return getFullTextTermIndex(handle, pos);
    }

    public long getFullTextTermStart(long pos) {
        return getFullTextTermStart(handle, pos);
    }

    public long getFullTextTermLength(long pos) {
        return getFullTextTermLength(handle, pos);
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
     * @param c4queryenumerator C4QueryEnumerator*
     * @return String (C4StringResult)
     * @throws LiteCoreException
     */
    private static native byte[] getFullTextMatched(long c4queryenumerator) throws LiteCoreException;

    private static native boolean next(long c4queryenumerator) throws LiteCoreException;

    private static native long getRowCount(long c4queryenumerator) throws LiteCoreException;

    private static native boolean seek(long c4queryenumerator, long rowIndex) throws LiteCoreException;

    private static native long refresh(long c4queryenumerator) throws LiteCoreException;

    private static native void close(long c4queryenumerator);

    private static native void free(long c4queryenumerator);

    // -- Accessor methods to C4QueryEnumerator --

    private static native String getDocID(long c4queryenumerator);

    private static native long getDocSequence(long c4queryenumerator);

    private static native String getRevID(long c4queryenumerator);

    private static native long getDocFlags(long c4queryenumerator);

    private static native long getColumns(long c4queryenumerator);

    private static native long getFullTextTermCount(long c4queryenumerator);

    private static native long getFullTextTermIndex(long c4queryenumerator, long pos);  // C4FullTextTerm.termIndex

    private static native long getFullTextTermStart(long c4queryenumerator, long pos);  // C4FullTextTerm.start

    private static native long getFullTextTermLength(long c4queryenumerator, long pos); // C4FullTextTerm.length
}
