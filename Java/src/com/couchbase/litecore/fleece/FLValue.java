package com.couchbase.litecore.fleece;

import com.couchbase.litecore.LiteCoreException;

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
    private long handle; // pointer to FLValue

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    public static FLValue fromData(byte[] data) {
        return new FLValue(fromTrustedData(data));
    }

    public FLValue(long handle) {
        this.handle = handle;
    }

    public int getType(){
        return getType(handle);
    }

    public boolean asBool() {
        return asBool(handle);
    }

    public long asUnsigned() {
        return asUnsigned(handle);
    }

    public int asInt() {
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

    public Object asObject() {
        switch (getType(handle)) {
            case kFLNull:
                return null;
            case kFLBoolean:
                return Boolean.valueOf(asBool());
            case kFLNumber:
                if (isInteger()) {
                    if (isUnsigned())
                        return Long.valueOf(asUnsigned());
                    return Integer.valueOf(asInt());
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

    /**
     * Converts valid JSON5 to JSON.
     * @param json5 String
     * @return JSON String
     * @throws LiteCoreException
     */
    public static String json5ToJson(String json5) throws LiteCoreException{
        return JSON5ToJSON(json5);
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
    private boolean isInteger() {
        return isInteger(handle);
    }

    private boolean isDouble() {
        return isDouble(handle);
    }

    private boolean isUnsigned() {
        return isUnsigned(handle);
    }

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------

    /**
     * Returns a pointer to the root value in the encoded data
     *
     * @param data FLSlice (same with slice)
     * @return long (FLValue - const struct _FLValue*)
     */
    private static native long fromTrustedData(byte[] data);

    /**
     * Returns the data type of an arbitrary Value.
     *
     * @param value FLValue
     * @return int (FLValueType)
     */
    private static native int getType(long value);

    /**
     * Is this value an integer?
     *
     * @param value FLValue
     * @return boolean
     */
    private static native boolean isInteger(long value);

    /**
     * Is this a 64-bit floating-point value?
     *
     * @param value FLValue
     * @return boolean
     */
    private static native boolean isDouble(long value);

    /**
     * Returns true if the value is non-nullptr and represents an _unsigned_ integer that can only
     * be represented natively as a `uint64_t`.
     *
     * @param value FLValue
     * @return boolean
     */
    private static native boolean isUnsigned(long value);

    /**
     * Returns a value coerced to boolean.
     *
     * @param value FLValue
     * @return boolean
     */
    private static native boolean asBool(long value);

    /**
     * Returns a value coerced to an unsigned integer.
     *
     * @param value FLValue
     * @return long
     */
    private static native long asUnsigned(long value);

    /**
     * Returns a value coerced to boolean.
     *
     * @param value FLValue
     * @return int
     */
    private static native int asInt(long value);

    /**
     * Returns a value coerced to a 32-bit floating point number.
     *
     * @param value FLValue
     * @return float
     */
    private static native float asFloat(long value);

    /**
     * Returns a value coerced to a 64-bit floating point number.
     *
     * @param value FLValue
     * @return double
     */
    private static native double asDouble(long value);

    /**
     * Returns the exact contents of a string value, or null for all other types.
     *
     * @param value FLValue
     * @return String
     */
    private static native String asString(long value);

    /**
     * Returns the exact contents of a data value, or null for all other types.
     *
     * @param value FLValue
     * @return byte[]
     */
    private static native byte[] asData(long value);

    /**
     * If a FLValue represents an array, returns it cast to FLArray, else nullptr.
     *
     * @param value FLValue
     * @return long (FLArray)
     */
    private static native long asArray(long value);

    /**
     * If a FLValue represents an array, returns it cast to FLArray, else nullptr.
     *
     * @param value FLValue
     * @return long (FLDict)
     */
    private static native long asDict(long value);

    // TODO: Need free()?????


    /**
     * Converts valid JSON5 to JSON.
     * @param json5 String
     * @return JSON String
     * @throws LiteCoreException
     */
    private static native String JSON5ToJSON(String json5) throws LiteCoreException;
}

