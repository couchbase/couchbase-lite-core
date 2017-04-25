package com.couchbase.litecore.fleece;

import com.couchbase.lite.SharedKeys;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;

public class FLDict {

    private long handle = 0; // hold pointer to FLDict

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public FLDict(long handle) {
        this.handle = handle;
    }

    public FLValue get(String key) {
        return new FLValue(get(handle, key == null ? null : key.getBytes()));
    }

    public FLValue getSharedKey(String key, FLSharedKeys sharedKeys) {
        if (key == null) return null;
        return new FLValue(getSharedKey(handle, key.getBytes(), sharedKeys.getHandle()));
    }

    public static String getKeyString(FLSharedKeys sharedKeys, int keyCode) {
        return getKeyString(sharedKeys.getHandle(), keyCode);
    }

    public FLValue getUnsorted(String key) {
        return new FLValue(getUnsorted(handle, key == null ? null : key.getBytes()));
    }

    /**
     * Return List of keys as Iterator
     * <p>
     * NOTE: If the approach of coping all keys does not work with large dataset,
     * need to concern the approach of binding to FLDictIterator.
     * However, Iterator does not have free() method. So need to wait to release FLDictIterator
     * till Iterator is garbage collected.
     */
    public Iterator<String> iterator(SharedKeys sharedKeys) {
        List<String> keys = new ArrayList<>();
        FLDictIterator itr = new FLDictIterator();
        itr.begin(this);
        String key;
        while ((key = SharedKeys.getKey(itr, sharedKeys)) != null) {
            //while ((key = itr.getKey().asString()) != null) {
            keys.add(key);
            itr.next();
        }

        itr.free();
        return keys.iterator();
    }

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

    public long count() {
        return count(handle);
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
     *
     * @param dict       FLDict
     * @param keyString  FLSlice
     * @param sharedKeys FLSharedKeys
     * @return FLValue
     */
    private static native long getSharedKey(long dict, byte[] keyString, long sharedKeys);

    private static native String getKeyString(long sharedKeys, int keyCode);

    private static native long getUnsorted(long dict, byte[] keyString);

    // TODO: Need free()?
}
