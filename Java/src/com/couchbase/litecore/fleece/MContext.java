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
