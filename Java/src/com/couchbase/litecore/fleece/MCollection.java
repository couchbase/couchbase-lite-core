package com.couchbase.litecore.fleece;

public class MCollection {
    long _handle;

    public MCollection() {
    }

    public MCollection(long handle) {
        _handle = handle;
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
        return new MContext(context(_handle));
    }

    public MCollection parent() {
        return new MCollection(parent(_handle));
    }

    @Override
    protected void finalize() throws Throwable {
        free();
        super.finalize();
    }

    // should be overridden
    protected void free() {
        // should be override
    }

    private static native boolean isMutable(long handle);

    private static native boolean isMutated(long handle);

    private static native boolean mutableChildren(long handle);

    private static native void setMutableChildren(long handle, boolean m);

    private static native long context(long handle);

    private static native long parent(long handle);
}
