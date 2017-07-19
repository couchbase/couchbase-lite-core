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

    public void setSharedKeys(FLSharedKeys flSharedKeys) {
        setSharedKeys(handle, flSharedKeys.getHandle());
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
            if (value instanceof Integer)
                return writeInt(handle, ((Integer) value).longValue());
            else if (value instanceof Long)
                return writeInt(handle, ((Long) value).longValue());
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

    static native long init(); // FLEncoder FLEncoder_New(void);

    static native void free(long encoder);

    static native void setSharedKeys(long encoder, long sharedKeys);

    static native boolean writeNull(long encoder);

    static native boolean writeBool(long encoder, boolean value);

    static native boolean writeInt(long encoder, long value); // 64bit

    static native boolean writeFloat(long encoder, float value);

    static native boolean writeDouble(long encoder, double value);

    static native boolean writeString(long encoder, String value);

    static native boolean writeData(long encoder, byte[] value);

    static native boolean beginArray(long encoder, long reserve);

    static native boolean endArray(long encoder);

    static native boolean beginDict(long encoder, long reserve);

    static native boolean endDict(long encoder);

    static native boolean writeKey(long encoder, String slice);

    static native boolean writeValue(long encoder, long value);

    static native byte[] finish(long encoder) throws LiteCoreException;
}
