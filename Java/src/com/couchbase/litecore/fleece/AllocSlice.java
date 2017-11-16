package com.couchbase.litecore.fleece;

public class AllocSlice {
    long _handle = 0;         // hold pointer to alloc_slice*
    boolean _managed = false; // true -> not release native object, false -> release by free()
    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public AllocSlice(byte[] bytes) {
        this(init(bytes), false);
    }

    public AllocSlice(long handle, boolean managed) {
        if (handle == 0)
            throw new IllegalArgumentException("handle is 0");
        this._handle = handle;
        this._managed = managed;
    }

    public void free() {
        if (_handle != 0L && !_managed) {
            free(_handle);
            _handle = 0L;
        }
    }

    public byte[] getBuf() {
        return getBuf(_handle);
    }

    public long getSize() {
        return getSize(_handle);
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

    static native long init(byte[] bytes);

    static native void free(long slice);

    static native byte[] getBuf(long slice);

    static native long getSize(long slice);
}
