package com.couchbase.litecore.fleece;

import com.couchbase.litecore.LiteCoreException;

import java.util.Iterator;
import java.util.List;
import java.util.Map;

public class Encoder {
    long _handle = 0; // hold pointer to Encoder*
    boolean _managed;

    public Encoder() {
        this(init(), false);
    }

    public Encoder(long handle, boolean managed) {
        if (handle == 0)
            throw new IllegalArgumentException();
        this._handle = handle;
        this._managed = managed;
    }

    public void free() {
        if (_handle != 0L && !_managed) {
            free(_handle);
            _handle = 0L;
        }
    }

    public void setSharedKeys(FLSharedKeys flSharedKeys) {
        setSharedKeys(_handle, flSharedKeys.getHandle());
    }

    public boolean writeNull() {
        return writeNull(_handle);
    }

    public boolean writeBool(boolean value) {
        return writeBool(_handle, value);
    }

    public boolean writeInt(long value) {
        return writeInt(_handle, value);
    }

    public boolean writeFloat(float value) {
        return writeFloat(_handle, value);
    }

    public boolean writeDouble(double value) {
        return writeDouble(_handle, value);
    }

    public boolean writeString(String value) {
        return writeString(_handle, value);
    }

    public boolean writeData(byte[] value) {
        return writeData(_handle, value);
    }

    public boolean beginDict(long reserve) {
        return beginDict(_handle, reserve);
    }

    public boolean endDict() {
        return endDict(_handle);
    }

    public boolean beginArray(long reserve) {
        return beginArray(_handle, reserve);
    }

    public boolean endArray() {
        return endArray(_handle);
    }

    public boolean writeKey(String slice) {
        return writeKey(_handle, slice);
    }

    public boolean writeValue(FLValue flValue) {
        return writeValue(_handle, flValue.getHandle());
    }

    public boolean write(Map map) {
        beginDict(map.size());
        Iterator keys = map.keySet().iterator();
        while (keys.hasNext()) {
            String key = (String) keys.next();
            writeKey(key);
            writeValue(map.get(key));
        }
        return endDict();
    }

    public boolean write(List list) {
        beginArray(list.size());
        for (Object item : list)
            writeValue(item);
        return endArray();
    }

    // C/Fleece+CoreFoundation.mm
    // bool FLEncoder_WriteNSObject(FLEncoder encoder, id obj)
    public boolean writeValue(Object value) {
        // null
        if (value == null)
            return writeNull(_handle);

            // boolean
        else if (value instanceof Boolean)
            return writeBool(_handle, (Boolean) value);

            // Number
        else if (value instanceof Number) {
            // Integer
            if (value instanceof Integer)
                return writeInt(_handle, ((Integer) value).longValue());

                // Long
            else if (value instanceof Long)
                return writeInt(_handle, ((Long) value).longValue());

                // Double
            else if (value instanceof Double)
                return writeDouble(_handle, ((Double) value).doubleValue());

                // Float
            else
                return writeFloat(_handle, ((Float) value).floatValue());
        }

        // String
        else if (value instanceof String)
            return writeString(_handle, (String) value);

            // byte[]
        else if (value instanceof byte[])
            return writeData(_handle, (byte[]) value);

            // List
        else if (value instanceof List)
            return write((List) value);

            // Map
        else if (value instanceof Map)
            return write((Map) value);

        return false;
    }

    public AllocSlice finish() throws LiteCoreException {
        return new AllocSlice(finish(_handle), false);
    }

    public byte[] finishAsBytes() {
        return finishAsBytes(_handle);
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

    static native long init();

    static native void free(long handle);

    static native void setSharedKeys(long handle, long sharedKeys);

    static native boolean writeNull(long handle);

    static native boolean writeBool(long handle, boolean value);

    static native boolean writeInt(long handle, long value); // 64bit

    static native boolean writeFloat(long handle, float value);

    static native boolean writeDouble(long handle, double value);

    static native boolean writeString(long handle, String value);

    static native boolean writeData(long handle, byte[] value);

    static native boolean writeValue(long handle, long value);

    static native boolean beginArray(long handle, long reserve);

    static native boolean endArray(long handle);

    static native boolean beginDict(long handle, long reserve);

    static native boolean writeKey(long handle, String slice);

    static native boolean endDict(long handle);

    static native byte[] finishAsBytes(long handle);

    static native long finish(long handle);
}
