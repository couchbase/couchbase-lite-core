package com.couchbase.litecore.fleece;

import com.couchbase.litecore.LiteCoreException;

import java.util.Iterator;
import java.util.List;
import java.util.Map;

public class FLEncoder {
    //-------------------------------------------------------------------------
    // private variable
    //-------------------------------------------------------------------------
    private long handle = 0;

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    public FLEncoder() {
        this.handle = init();
    }

    public FLEncoder(long handle) {
        this.handle = handle;
    }

    public void free() {
        if (handle != 0) {
            free(handle);
            handle = 0;
        }
    }

    public boolean writeNull() {
        return writeNull(handle);
    }

    public boolean writeBool(boolean value) {
        return writeBool(handle, value);
    }

    public boolean writeInt(long value) {
        return writeInt(handle, value);
    }

    public boolean writeFloat(float value) {
        return writeFloat(handle, value);
    }

    public boolean writeDouble(double value) {
        return writeDouble(handle, value);
    }

    public boolean writeString(String value) {
        return writeString(handle, value);
    }

    public boolean writeData(byte[] value) {
        return writeData(handle, value);
    }

    public boolean beginDict(long reserve) {
        return beginDict(handle, reserve);
    }

    public boolean endDict() {
        return endDict(handle);
    }

    public boolean beginArray(long reserve) {
        return beginArray(handle, reserve);
    }

    public boolean endArray() {
        return endArray(handle);
    }

    public boolean writeKey(String slice) {
        return writeKey(handle, slice);
    }

    public boolean writeValue(FLValue flValue) {
        return writeValue(handle, flValue.getHandle());
    }

    // C/Fleece+CoreFoundation.mm
    // bool FLEncoder_WriteNSObject(FLEncoder encoder, id obj)
    public boolean writeValue(Object value) {
        if (value == null)
            return writeNull(handle);
        else if (value instanceof Boolean)
            return writeBool(handle, (Boolean) value);
        else if (value instanceof Number) {
            if (value instanceof Integer || value instanceof Long)
                return writeInt(handle, ((Number) value).longValue());
            else if (value instanceof Double)
                return writeDouble(handle, ((Double) value).doubleValue());
            else
                return writeFloat(handle, ((Float) value).floatValue());
        } else if (value instanceof String)
            return writeString(handle, (String) value);
        else if (value instanceof byte[])
            return writeData(handle, (byte[]) value);
        else if (value instanceof List)
            return write((List) value);
        else if (value instanceof Map)
            return write((Map) value);
        return false;
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
        for (Object item : list) {
            writeValue(item);
        }
        return endArray();
    }

    public byte[] finish() throws LiteCoreException {
        return finish(handle);
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
    // private methods
    //-------------------------------------------------------------------------

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------

    private native static long init(); // FLEncoder FLEncoder_New(void);

    private native static void free(long encoder);

    private native static boolean writeNull(long encoder);

    private native static boolean writeBool(long encoder, boolean value);

    private native static boolean writeInt(long encoder, long value); // 64bit

    private native static boolean writeFloat(long encoder, float value);

    private native static boolean writeDouble(long encoder, double value);

    private native static boolean writeString(long encoder, String value);

    private native static boolean writeData(long encoder, byte[] value);

    private native static boolean beginArray(long encoder, long reserve);

    private native static boolean endArray(long encoder);

    private native static boolean beginDict(long encoder, long reserve);

    private native static boolean endDict(long encoder);

    private native static boolean writeKey(long encoder, String slice);

    private native static boolean writeValue(long encoder, long value);

    private native static byte[] finish(long encoder) throws LiteCoreException;
}
