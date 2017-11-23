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
    long _handle = 0L; // hold pointer to C4QueryEnumerator

    //-------------------------------------------------------------------------
    // Constructor
    //-------------------------------------------------------------------------
    C4QueryEnumerator(long handle) {
        this._handle = handle;
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    public boolean next() throws LiteCoreException {
        boolean ok = next(_handle);
        // NOTE: Please keep folowing line of code for a while.
        //if (!ok)
        //    handle = 0;
        return ok;
    }

    public long getRowCount() throws LiteCoreException {
        return getRowCount(_handle);
    }

    public boolean seek(long rowIndex) throws LiteCoreException {
        return seek(_handle, rowIndex);
    }

    public C4QueryEnumerator refresh() throws LiteCoreException {
        // handle is closed or reached end.
        if (_handle == 0)
            return null;
        long newHandle = refresh(_handle);
        if (newHandle == 0)
            return null;
        return new C4QueryEnumerator(newHandle);
    }

    public void close() {
        if (_handle != 0)
            close(_handle);
    }

    public boolean isClosed() {
        return _handle == 0;
    }

    public void free() {
        if (_handle != 0L) {
            free(_handle);
            _handle = 0L;
        }
    }

    // -- Accessor methods to C4QueryEnumerator --

    // NOTE: FLArrayIterator is member variable of C4QueryEnumerator. Not necessary to release.
    public FLArrayIterator getColumns() {
        return new FLArrayIterator(getColumns(_handle));
    }

    public long getFullTextMatchCount() {
        return getFullTextMatchCount(_handle);
    }

    public C4FullTextMatch getFullTextMatchs(int idx) {
        return new C4FullTextMatch(getFullTextMatch(_handle, idx));
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

    static native boolean next(long handle) throws LiteCoreException;

    static native long getRowCount(long handle) throws LiteCoreException;

    static native boolean seek(long handle, long rowIndex) throws LiteCoreException;

    static native long refresh(long handle) throws LiteCoreException;

    static native void close(long handle);

    static native void free(long handle);

    // -- Accessor methods to C4QueryEnumerator --

    // C4QueryEnumerator.columns
    static native long getColumns(long handle);

    // C4QueryEnumerator.fullTextMatchCount
    static native long getFullTextMatchCount(long handle);

    // C4QueryEnumerator.fullTextMatches
    static native long getFullTextMatch(long handle, int idx);
}
