//
// FLValue.java
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
package com.couchbase.litecore.fleece;

import com.couchbase.litecore.LiteCoreException;
import com.couchbase.litecore.SharedKeys;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import static com.couchbase.litecore.fleece.FLConstants.FLValueType.kFLArray;
import static com.couchbase.litecore.fleece.FLConstants.FLValueType.kFLBoolean;
import static com.couchbase.litecore.fleece.FLConstants.FLValueType.kFLData;
import static com.couchbase.litecore.fleece.FLConstants.FLValueType.kFLDict;
import static com.couchbase.litecore.fleece.FLConstants.FLValueType.kFLNull;
import static com.couchbase.litecore.fleece.FLConstants.FLValueType.kFLNumber;
import static com.couchbase.litecore.fleece.FLConstants.FLValueType.kFLString;

/**
 * FLValue_xxxx(...)
 * Fleece.h
 */

public class FLValue {
    //-------------------------------------------------------------------------
    // private variables
    //-------------------------------------------------------------------------
    private long handle = 0L; // pointer to FLValue

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public static FLValue fromData(AllocSlice slice) {
        long value = fromData(slice._handle);
        return value != 0 ? new FLValue(value) : null;
    }

    public static FLValue fromData(byte[] data) {
        return new FLValue(fromTrustedData(data));
    }

    public static FLValue fromTrustedData(FLSliceResult data) {
        return new FLValue(fromTrustedData2(data.getHandle()));
    }

    public FLValue(long handle) {
        if (handle == 0L) throw new IllegalArgumentException("handle is 0L.");
        this.handle = handle;
    }

    public FLValue(FLArray flArray) {
        this.handle = flArray.getHandle();
    }

    public int getType() {
        return getType(handle);
    }

    public boolean asBool() {
        return asBool(handle);
    }

    public long asUnsigned() {
        return asUnsigned(handle);
    }

    public long asInt() {
        return asInt(handle);
    }

    public byte[] asData() {
        return asData(handle);
    }

    public float asFloat() {
        return asFloat(handle);
    }

    public double asDouble() {
        return asDouble(handle);
    }

    public String asString() {
        return asString(handle);
    }

    public FLDict asFLDict() {
        return new FLDict(asDict(handle));
    }

    public FLArray asFLArray() {
        return new FLArray(asArray(handle));
    }

    public Map<String, Object> asDict() {
        return asFLDict().asDict();
    }

    public List<Object> asArray() {
        return asFLArray().asArray();
    }

    // call from native (JNI)
    private static Object asObject(long h) {
        return asObject(new FLValue(h));
    }

    private static Object asObject(FLValue flValue) {
        return flValue.asObject();
    }


    Object asObject() {
        switch (getType(handle)) {
            case kFLNull:
                return null;
            case kFLBoolean:
                return Boolean.valueOf(asBool());
            case kFLNumber:
                if (isInteger()) {
                    if (isUnsigned())
                        return Long.valueOf(asUnsigned());
                    return Long.valueOf(asInt());
                } else if (isDouble()) {
                    return Double.valueOf(asDouble());
                } else {
                    return Float.valueOf(asFloat());
                }
            case kFLString:
                return asString();
            case kFLData:
                return asData();
            case kFLArray:
                return asArray();
            case kFLDict: {
                return asDict();
            }
            default:
                return null;
        }
    }

    // call from native (JNI)
    static Object toObject(long h) {
        return toObject(new FLValue(h));
    }

    public static Object toObject(FLValue flValue) {
        return flValue.toObject((SharedKeys) null);
    }

    // TODO: This method should be in `com.couchbase.lite.Properties` or `com.couchbase.lite.Document` ?
    // Equivalent to
    // in Value+ObjC.mm
    // id Value::toNSObject(__unsafe_unretained NSMapTable *sharedStrings, const SharedKeys *sk) const
    // In FLValueConverter.cs
    // public static object ToObject(FLValue* value, SharedStringCache sharedKeys)
    public Object toObject(SharedKeys sharedKeys) {
        switch (getType(handle)) {
            case kFLNull:
                return null;
            case kFLBoolean:
                return Boolean.valueOf(asBool());
            case kFLNumber:
                if (isInteger()) {
                    if (isUnsigned())
                        return Long.valueOf(asUnsigned());
                    return Long.valueOf(asInt());
                } else if (isDouble()) {
                    return Double.valueOf(asDouble());
                } else {
                    return Float.valueOf(asFloat());
                }
            case kFLString:
                return asString();
            case kFLData:
                return asData();
            case kFLArray: {
                List<Object> results = new ArrayList<>();
                FLArrayIterator itr = new FLArrayIterator();
                try {
                    itr.begin(asFLArray());
                    FLValue value;
                    while ((value = itr.getValue()) != null) {
                        results.add(value.toObject(sharedKeys));
                        if (!itr.next())
                            break;
                    }
                } finally {
                    itr.free();
                }
                return results;
            }
            case kFLDict: {
                FLDict dict = asFLDict();
                if (sharedKeys == null) {
                    return dict.asDict();
                } else {
                    Map<String, Object> results = new HashMap<>();
                    FLDictIterator itr = new FLDictIterator();
                    try {
                        itr.begin(dict);
                        String key;
                        while ((key = SharedKeys.getKey(itr, sharedKeys)) != null) {
                            Object value = itr.getValue().toObject(sharedKeys);
                            results.put(key, value);
                            itr.next();
                        }
                    } finally {
                        itr.free();
                    }
                    return results;
                }
            }
            default:
                return null; // TODO: Throw Exeption?
        }
    }

    /**
     * Converts valid JSON5 to JSON.
     *
     * @param json5 String
     * @return JSON String
     * @throws LiteCoreException
     */
    public static String json5ToJson(String json5) throws LiteCoreException {
        return JSON5ToJSON(json5);
    }

    public boolean isNumber() {
        return getType() == kFLNumber;
    }

    public boolean isInteger() {
        return isInteger(handle);
    }

    public boolean isDouble() {
        return isDouble(handle);
    }

    public boolean isUnsigned() {
        return isUnsigned(handle);
    }

    // @note instead of toString()
    public String toStr() {
        return toString(handle);
    }

    public String toJSON() {
        return toJSON(handle);
    }

    public String toJSON5() {
        return toJSON5(handle);
    }

    //-------------------------------------------------------------------------
    // package level access
    //-------------------------------------------------------------------------
    long getHandle() {
        return handle;
    }

    //-------------------------------------------------------------------------
    // private methods
    //-------------------------------------------------------------------------


    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------

    static native long fromData(long slice);

    /**
     * Returns a pointer to the root value in the encoded data
     *
     * @param data FLSlice (same with slice)
     * @return long (FLValue - const struct _FLValue*)
     */
    static native long fromTrustedData(byte[] data);

    /**
     * Returns a pointer to the root value in the encoded data
     *
     * @param data pointer to FLSlice (same with slice)
     * @return long (FLValue - const struct _FLValue*)
     */
    static native long fromTrustedData2(long data);

    /**
     * Returns the data type of an arbitrary Value.
     *
     * @param value FLValue
     * @return int (FLValueType)
     */
    static native int getType(long value);

    /**
     * Is this value an integer?
     *
     * @param value FLValue
     * @return boolean
     */
    static native boolean isInteger(long value);

    /**
     * Is this a 64-bit floating-point value?
     *
     * @param value FLValue
     * @return boolean
     */
    static native boolean isDouble(long value);

    /**
     * Returns true if the value is non-nullptr and represents an _unsigned_ integer that can only
     * be represented natively as a `uint64_t`.
     *
     * @param value FLValue
     * @return boolean
     */
    static native boolean isUnsigned(long value);

    /**
     * Returns a value coerced to boolean.
     *
     * @param value FLValue
     * @return boolean
     */
    static native boolean asBool(long value);

    /**
     * Returns a value coerced to an unsigned integer.
     *
     * @param value FLValue
     * @return long
     */
    static native long asUnsigned(long value);

    /**
     * Returns a value coerced to an integer.
     * NOTE: litecore treats integer with 2^64. So this JNI method returns long value
     *
     * @param value FLValue
     * @return long
     */
    static native long asInt(long value);

    /**
     * Returns a value coerced to a 32-bit floating point number.
     *
     * @param value FLValue
     * @return float
     */
    static native float asFloat(long value);

    /**
     * Returns a value coerced to a 64-bit floating point number.
     *
     * @param value FLValue
     * @return double
     */
    static native double asDouble(long value);

    /**
     * Returns the exact contents of a string value, or null for all other types.
     *
     * @param value FLValue
     * @return String
     */
    static native String asString(long value);

    /**
     * Returns the exact contents of a data value, or null for all other types.
     *
     * @param value FLValue
     * @return byte[]
     */
    static native byte[] asData(long value);

    /**
     * If a FLValue represents an array, returns it cast to FLArray, else nullptr.
     *
     * @param value FLValue
     * @return long (FLArray)
     */
    static native long asArray(long value);

    /**
     * If a FLValue represents an array, returns it cast to FLDict, else nullptr.
     *
     * @param value FLValue
     * @return long (FLDict)
     */
    static native long asDict(long value);

    /**
     * Converts valid JSON5 to JSON.
     *
     * @param json5 String
     * @return JSON String
     * @throws LiteCoreException
     */
    static native String JSON5ToJSON(String json5) throws LiteCoreException;


    static native String toString(long handle);

    static native String toJSON(long handle);

    static native String toJSON5(long handle);


    // TODO: Need free()?

    //static native Object asObject(long h);
}

