package com.couchbase.litecore.fleece;

public class MRoot extends MCollection {

    public MRoot(MContext context, FLValue flValue, boolean  isMutable){
        super(initWithContext(context._handle, flValue.getHandle(), isMutable), false);
    }

    public MRoot(AllocSlice fleeceData) {
        this(fleeceData, null);
    }

    MRoot(AllocSlice fleeceData, FLSharedKeys sk) {
        this(fleeceData, sk, true);
    }

    public MRoot(AllocSlice fleeceData, FLSharedKeys sk, boolean isMutable) {
        super(init(fleeceData._handle, sk != null ? sk.handle : 0L, true), false);
    }

    @Override
    public void free() {
        if (_handle != 0L && !_managed) {
            free(_handle);
            _handle = 0L;
        }
    }

    public static Object toNative(AllocSlice fleeceData) {
        return toNative(fleeceData._handle, 0L, true);
    }

    public static Object toNative(AllocSlice fleeceData, FLSharedKeys sk) {
        return toNative(fleeceData._handle, sk != null ? sk.handle : 0L, true);
    }

    public static Object toNative(AllocSlice fleeceData, FLSharedKeys sk, boolean mutableContainers) {
        return toNative(fleeceData._handle, sk != null ? sk.handle : 0L, mutableContainers);
    }

    public MContext context() {
        return new MContext(context(_handle), true);
    }

    public Object asNative() {
        return asNative(_handle);
    }

    public boolean isMutated() {
        return isMutated(_handle);
    }

    public void encodeTo(Encoder enc) {
        encodeTo(_handle, enc._handle);
    }

    public AllocSlice encode() {
        return new AllocSlice(encode(_handle), false);
    }

    public AllocSlice encodeDelta() {
        return new AllocSlice(encodeDelta(_handle), false);
    }

    @Override
    protected void finalize() throws Throwable {
        free();
        super.finalize();
    }

    // static Native asNative(alloc_slice, FLSharedKeys, bool)
    private static native Object toNative(long fleeceData, long sk, boolean mutableContainers);

    private static native long initWithContext(long context, long value, boolean isMutable);

    private static native long init(long fleeceData, long sk, boolean isMutable);

    private static native void free(long handle);

    private static native long context(long handle);

    private static native Object asNative(long handle);

    private static native boolean isMutated(long handle);

    private static native void encodeTo(long handle, long enc);

    private static native long encode(long handle);

    private static native long encodeDelta(long handle);
}
