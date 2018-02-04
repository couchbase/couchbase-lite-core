//
// SharedKeys.java
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
package com.couchbase.litecore;

import com.couchbase.litecore.fleece.FLDict;
import com.couchbase.litecore.fleece.FLDictIterator;
import com.couchbase.litecore.fleece.FLEncoder;
import com.couchbase.litecore.fleece.FLSharedKeys;
import com.couchbase.litecore.fleece.FLSliceResult;
import com.couchbase.litecore.fleece.FLValue;

public final class SharedKeys {

    //---------------------------------------------
    // member variables
    //---------------------------------------------
    private final FLSharedKeys flSharedKeys;
    private final Object lock = new Object();

    //---------------------------------------------
    // Constructors
    //---------------------------------------------
    public SharedKeys(final C4Database c4db) {
        flSharedKeys = c4db.getFLSharedKeys();
    }

    public SharedKeys(final FLSharedKeys flSharedKeys) {
        this.flSharedKeys = flSharedKeys;
    }
    //---------------------------------------------
    // Package level methods
    //---------------------------------------------

    // id __nullable valueToObject(FLValue __nullable value)
    Object valueToObject(final FLValue value) {
        synchronized (lock) {
            return value == null ? null : value.toObject(this);
        }
    }

    // FLValue getDictValue(FLDict __nullable dict, FLSlice key)
    FLValue getValue(final FLDict dict, final String key) {
        synchronized (lock) {
            return dict.getSharedKey(key, flSharedKeys);
        }
    }

    // NSString* getDictIterKey(FLDictIterator* iter)
    public String getDictIterKey(final FLDictIterator itr) {
        synchronized (lock) {
            FLValue key = itr.getKey();
            if (key == null)
                return null;

            if (key.isNumber())
                return getKey((int) key.asInt());

            Object obj = valueToObject(key);
            if (obj instanceof String)
                return (String) obj;
            else if (obj == null)
                return null;
            else
                throw new IllegalStateException("obj is not String: obj type -> " + obj.getClass().getSimpleName());
        }
    }

    // public string GetKey(int index)
    public String getKey(final int index) {
        synchronized (lock) {
            return FLDict.getKeyString(flSharedKeys, index);
        }
    }

    // bool encodeKey(FLEncoder encoder, FLSlice key)
    boolean encodeKey(final FLEncoder encoder, final String key) {
        synchronized (lock) {
            return encoder.writeKey(key);
        }
    }

    // bool encodeValue(FLEncoder encoder, FLValue __nullable value)
    boolean encodeValue(final FLEncoder encoder, final FLValue value) {
        synchronized (lock) {
            return encoder.writeValueWithSharedKeys(value, flSharedKeys);
        }
    }

    public boolean dictContainsBlobs(final FLSliceResult dict) {
        synchronized (lock) {
            return C4Document.dictContainsBlobs(dict, flSharedKeys);
        }
    }

    public FLSharedKeys getFLSharedKeys() {
        return flSharedKeys;
    }
    //---------------------------------------------
    // static methods
    //---------------------------------------------

    // static inline id FLValue_GetNSObject(FLValue __nullable value, cbl::SharedKeys *sk)
    public static Object valueToObject(final FLValue value, final SharedKeys sk) {
        if (sk == null) throw new RuntimeException("sk is null");
        return sk != null ? sk.valueToObject(value) : null;
    }

    // static inline FLValue FLDict_GetSharedKey
    //      (FLDict __nullable dict, FLSlice key, cbl::SharedKeys *sk)
    public static FLValue getValue(final FLDict dict, final String key, final SharedKeys sk) {
        if (sk == null) throw new RuntimeException("sk is null");
        return sk != null && dict != null ? sk.getValue(dict, key) : null;
    }

    // static inline NSString* FLDictIterator_GetKey(FLDictIterator *iter, cbl::SharedKeys *sk)
    public static String getKey(final FLDictIterator itr, final SharedKeys sk) {
        if (sk == null) throw new RuntimeException("sk is null");
        return sk != null && itr != null ? sk.getDictIterKey(itr) : null;
    }

    // static inline bool FL_WriteKey(FLEncoder encoder, FLSlice key, cbl::SharedKeys *sk)
    public static boolean writeKey(final FLEncoder encoder, final String key, final SharedKeys sk) {
        if (sk == null) throw new RuntimeException("sk is null");
        return sk != null ? sk.encodeKey(encoder, key) : false;
    }

    // static inline bool FL_WriteValue(FLEncoder encoder, FLValue __nullable value, cbl::SharedKeys *sk)
    public static boolean writeValue(final FLEncoder encoder, final FLValue value, final SharedKeys sk) {
        if (sk == null) throw new RuntimeException("sk is null");
        return sk != null ? sk.encodeValue(encoder, value) : false;
    }

    public static boolean dictContainsBlobs(final FLSliceResult dict, final SharedKeys sk) {
        if (sk == null) throw new RuntimeException("sk is null");
        return sk.dictContainsBlobs(dict);
    }

}

