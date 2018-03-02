//
// FLDict.java
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

import com.couchbase.litecore.SharedKeys;

import java.util.HashMap;
import java.util.Map;

public class FLDict {

    private long handle = 0; // hold pointer to FLDict

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public FLDict(long handle) {
        this.handle = handle;
    }

    public FLValue getSharedKey(String key, FLSharedKeys sharedKeys) {
        if (key == null) return null;
        long hValue = getSharedKey(handle, key.getBytes(), sharedKeys == null ? 0L : sharedKeys.getHandle());
        return hValue != 0L ? new FLValue(hValue) : null;
    }

    public static String getKeyString(FLSharedKeys sharedKeys, int keyCode) {
        return getKeyString(sharedKeys == null ? 0L : sharedKeys.getHandle(), keyCode);
    }

    public Map<String, Object> asDict() {
        Map<String, Object> results = new HashMap<>();
        FLDictIterator itr = new FLDictIterator();
        try {
            itr.begin(this);
            FLValue flKey;
            while ((flKey = itr.getKey()) != null) {
                String key = flKey.asString();
                Object value = itr.getValue().asObject();
                results.put(key, value);
                if (!itr.next())
                    break;
            }
        } finally {
            itr.free();
        }
        return results;
    }

    public Map<String, Object> toObject(final SharedKeys sharedKeys) {
        Map<String, Object> results = new HashMap<>();
        FLDictIterator itr = new FLDictIterator();
        try {
            itr.begin(this);
            String key;
            while ((key = SharedKeys.getKey(itr, sharedKeys)) != null) {
                Object value = itr.getValue().toObject(sharedKeys);
                results.put(key, value);
                if (!itr.next())
                    break;
            }
        } finally {
            itr.free();
        }
        return results;
    }

    public long count() {
        if (handle == 0L) throw new IllegalStateException("handle is 0L");
        return count(handle);
    }

    public FLValue toFLValue() {
        return new FLValue(handle);
    }

    //-------------------------------------------------------------------------
    // package level access
    //-------------------------------------------------------------------------
    public long getHandle() {
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
    static native long count(long dict);

    /**
     * Looks up a key in a _sorted_ dictionary, using a shared-keys mapping.
     *
     * @param dict       FLDict
     * @param keyString  FLSlice
     * @param sharedKeys FLSharedKeys
     * @return FLValue
     */
    static native long getSharedKey(long dict, byte[] keyString, long sharedKeys);

    static native String getKeyString(long sharedKeys, int keyCode);
}
