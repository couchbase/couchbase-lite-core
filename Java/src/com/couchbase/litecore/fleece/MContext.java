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

public class MContext {
    protected long _handle;
    protected boolean _managed;

    public MContext(AllocSlice data, FLSharedKeys sk) {
        _handle = init(data._handle, sk.getHandle());
        _managed = false;
    }

    public MContext(long handle, boolean managed) {
        _handle = handle;
        _managed = managed;
    }

    public void free() {
        if (_handle != 0L && !_managed) {
            free(_handle);
            _handle = 0L;
        }
    }

    public FLSharedKeys sharedKeys() {
        return new FLSharedKeys(sharedKeys(_handle));
    }

    public void setNative(Object obj) {
        setNative(_handle, obj);
    }

    public Object getNative() {
        return getNative(_handle);
    }

    @Override
    protected void finalize() throws Throwable {
        free();
        super.finalize();
    }

    private static native long init(long data, long sk);

    private static native void free(long handle);

    private static native long sharedKeys(long handle);

    private static native void setNative(long handle, Object obj);

    private static native Object getNative(long handle);
}
