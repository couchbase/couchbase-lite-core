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

public class MCollection {
    long _handle = 0L;
    boolean _managed = false;

    public MCollection(long handle, boolean managed) {
        _handle = handle;
        _managed = managed;
    }

    public boolean isMutable() {
        return isMutable(_handle);
    }

    public boolean isMutated() {
        return isMutated(_handle);
    }

    public boolean mutableChildren() {
        return mutableChildren(_handle);
    }

    public void setMutableChildren(boolean m) {
        setMutableChildren(_handle, m);
    }

    public MContext context() {
        return new MContext(context(_handle), true);
    }

    public MCollection parent() {
        return new MCollection(parent(_handle), true);
    }

    @Override
    protected void finalize() throws Throwable {
        free();
        super.finalize();
    }

    // should be overridden
    public void free() {
    }

    private static native boolean isMutable(long handle);

    private static native boolean isMutated(long handle);

    private static native boolean mutableChildren(long handle);

    private static native void setMutableChildren(long handle, boolean m);

    private static native long context(long handle);

    private static native long parent(long handle);
}
