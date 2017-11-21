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
    C4QueryEnumerator(long handle) {
        this.handle = handle;
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

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

    // NOTE: FLArrayIterator is member variable of C4QueryEnumerator. Not necessary to release.
    public FLArrayIterator getColumns() {
        return new FLArrayIterator(getColumns(handle));
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

    static native boolean next(long c4queryenumerator) throws LiteCoreException;

    static native long getRowCount(long c4queryenumerator) throws LiteCoreException;

    static native boolean seek(long c4queryenumerator, long rowIndex) throws LiteCoreException;

    static native long refresh(long c4queryenumerator) throws LiteCoreException;

    static native void close(long c4queryenumerator);

    static native void free(long c4queryenumerator);

    // -- Accessor methods to C4QueryEnumerator --

    static native long getColumns(long c4queryenumerator);
}
