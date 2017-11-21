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
package com.couchbase.litecore.fleece;

public class MArray extends MCollection {
    public MArray() {
        super(init(), false);
    }

    public void initInSlot(MValue mv, MCollection parent) {
        initInSlot(mv._handle, parent._handle);
    }

    void initInSlot(long hMv, long hParent) {
        initInSlot(_handle, hMv, hParent);
    }

    public void initAsCopyOf(MArray a, boolean isMutable) {
        initAsCopyOf(a._handle, isMutable);
    }

    void initAsCopyOf(long hMDict, boolean isMutable) {
        initAsCopyOf(_handle, hMDict, isMutable);
    }

    public long count() {
        return count(_handle);
    }

    public MValue get(int i) {
        return new MValue(get(_handle, i));
    }

    public boolean set(int i, Object val) {
        return set(_handle, i, val);
    }

    public boolean insert(int i, Object val) {
        return insert(_handle, i, val);
    }

    public boolean append(Object val) {
        return append(_handle, val);
    }

    public boolean remove(int i) {
        return remove(_handle, i, 1);
    }

    public boolean remove(int i, int n) {
        return remove(_handle, i, n);
    }

    public boolean clear() {
        return clear(_handle);
    }

    public void encodeTo(Encoder enc) {
        encodeTo(_handle, enc._handle);
    }

    @Override
    protected void finalize() throws Throwable {
        free();
        super.finalize();
    }

    @Override
    public void free() {
        if (_handle != 0L && !_managed) {
            free(_handle);
            _handle = 0L;
        }
    }

    private static native void free(long handle);

    private static native long init();

    private static native void initInSlot(long handle, long mv, long parent);

    private static native void initAsCopyOf(long handle, long mDict, boolean isMutable);

    private static native long count(long handle);

    private static native long get(long handle, int i);

    private static native boolean set(long handle, int i, Object val);

    private static native boolean insert(long handle, int i, Object val);

    private static native boolean append(long handle, Object val);

    private static native boolean remove(long handle, int i, int n);

    private static native boolean clear(long handle);

    private static native void encodeTo(long handle, long encoder);
}
