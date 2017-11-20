package com.couchbase.litecore.fleece;

public class MValue {
    long _handle;
    boolean _managed;

    public MValue(Object n) {
        this(init(n), false);
    }

    public MValue(long handle) {
        this(handle, true);
    }

    public MValue(long handle, boolean managed) {
        _handle = handle;
        _managed = managed;
    }

    public FLValue value() {
        long val = value(_handle);
        return val != 0 ? new FLValue(val) : null;
    }

    public boolean isEmpty() {
        return isEmpty(_handle);
    }

    public boolean isMutated() {
        return isMutated(_handle);
    }

    public boolean hasNative() {
        return hasNative(_handle);
    }

    public Object asNative(MCollection parent) {
        if (isEmpty()) return null;
        return asNative(_handle, parent != null ? parent._handle : 0L);
    }

    public void encodeTo(Encoder enc) {
        encodeTo(_handle, enc._handle);
    }

    public void mutate() {
        mutate(_handle);
    }

    public void free() {
        if (_handle != 0L && !_managed) {
            free(_handle);
            _handle = 0L;
        }
    }

    @Override
    protected void finalize() throws Throwable {
        free();
        super.finalize();
    }

    private static long getFLCollection(Object obj) {
        if (obj != null && obj instanceof FLCollection)
            return ((FLCollection) obj).getFLCollectionHandle();
        else
            return 0L;
    }

    private static void encodeNative(long hEncoder, Object obj) {
        if (hEncoder == 0L) return;
        Encoder encoder = new Encoder(hEncoder, true);
        encoder.writeObject(obj);
    }

    private static native void free(long handle);

    private static native long init(Object n);

    private static native long value(long handle);

    private static native boolean isEmpty(long handle);

    private static native boolean isMutated(long handle);

    private static native boolean hasNative(long handle);

    private static native Object asNative(long handle, long parent);

    private static native void encodeTo(long handle, long enc);

    private static native void mutate(long handle);
}
