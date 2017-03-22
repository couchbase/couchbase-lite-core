package com.couchbase.litecore.fleece;

import java.util.HashMap;
import java.util.Map;

public class FLDict {

    private long handle; // hold pointer to FLDict

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public FLDict(long handle) {
        this.handle = handle;
    }

    public FLValue get(String key) {
        return new FLValue(get(handle, key == null ? null : key.getBytes()));
    }

    // TODO: DB005
//    public FLValue getSharedKey(String key, long sharedKeys){
//        return new FLValue(getSharedKey(handle, key.getBytes(), sharedKeys));
//    }

    public Map<String, Object> asDict() {
        Map<String, Object> results = new HashMap<>();
        FLDictIterator itr = new FLDictIterator();
        itr.begin(this);
        String key;
        while ((key = itr.getKey().asString()) != null) {
            Object value = itr.getValue().asObject();
            results.put(key, value);
            itr.next();
        }
        itr.free();
        return results;
    }

    //-------------------------------------------------------------------------
    // package level access
    //-------------------------------------------------------------------------
    long getHandle() {
        return handle;
    }

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------

    /**
     * Returns the number of items in a dictionary, or 0 if the pointer is nullptr.
     *
     * @param dict FLDict
     * @return uint32_t
     */
    private static native long count(long dict);

    /**
     * Looks up a key in a _sorted_ dictionary, returning its value.
     *
     * @param dict      FLDict
     * @param keyString FLSlice
     * @return FLValue
     */
    private static native long get(long dict, byte[] keyString);

    /**
     * Looks up a key in a _sorted_ dictionary, using a shared-keys mapping.
     * @param dict FLDict
     * @param keyString FLSlice
     * @param sharedKeys FLSharedKeys
     * @return FLValue
     */
    // TODO:
    //private static native long getSharedKey(long dict, byte[] keyString, long sharedKeys);
}
