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

public class MDictIterator {
    long _handle;
    boolean _managed;

    public MDictIterator(MDict dict) {
        _handle = init(dict._handle);
        _managed = false;
    }

    public String key() {
        return key(_handle);
    }

    public Object value() {
        return value(_handle);
    }

    public boolean next() {
        return next(_handle);
    }

    @Override
    protected void finalize() throws Throwable {
        free();
        super.finalize();
    }

    protected void free() {
        if (_handle != 0L && !_managed) {
            free(_handle);
            _handle = 0L;
        }
    }

    private static native void free(long handle);

    private static native long init(long hMDict);

    private static native String key(long handle);

    private static native Object value(long handle);

    private static native boolean next(long handle);
}
