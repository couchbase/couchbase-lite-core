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

public class MDict extends MCollection {
    public MDict() {
        super(init(), false);
    }

    public void initInSlot(MValue mv, MCollection parent) {
        initInSlot(mv._handle, parent._handle);
    }

    public void initInSlot(long hMv, long hParent) {
        initInSlot(_handle, hMv, hParent);
    }

    public void initAsCopyOf(MDict mDict, boolean isMutable) {
        initAsCopyOf(mDict._handle, isMutable);
    }

    public void initAsCopyOf(long hMDict, boolean isMutable) {
        initAsCopyOf(_handle, hMDict, isMutable);
    }

    public long count() {
        return count(_handle);
    }

    public boolean contains(String key) {
        return contains(_handle, key);
    }

    public MValue get(String key) {
        return new MValue(get(_handle, key));
    }

    public boolean set(String key, Object val) {
        return set(key, new MValue(val));
    }

    public boolean set(String key, MValue val) {
        return set(_handle, key, val._handle);
    }

    public boolean remove(String key) {
        return remove(_handle, key);
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

    private static native boolean contains(long handle, String key);

    private static native long get(long handle, String key);

    private static native boolean set(long handle, String key, long val);

    private static native boolean remove(long handle, String key);

    private static native boolean clear(long handle);

    private static native void encodeTo(long handle, long encoder);
}
