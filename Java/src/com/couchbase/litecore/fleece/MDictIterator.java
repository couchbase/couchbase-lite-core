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
