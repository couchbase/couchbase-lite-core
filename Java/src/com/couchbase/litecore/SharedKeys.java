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
import com.couchbase.litecore.fleece.FLSharedKeys;
import com.couchbase.litecore.fleece.FLSliceResult;
import com.couchbase.litecore.fleece.FLValue;

public final class SharedKeys {

    //---------------------------------------------
    // member variables
    //---------------------------------------------
    private final FLSharedKeys flSharedKeys;

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
    // Public level methods
    //---------------------------------------------
    public FLSharedKeys getFLSharedKeys() {
        return flSharedKeys;
    }

    //---------------------------------------------
    // Package level methods
    //---------------------------------------------

    Object valueToObject(final FLValue value) {
        return value == null ? null : value.toObject(this);
    }

    String getDictIterKey(final FLDictIterator itr) {
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

    String getKey(final int index) {
        // NOTE: synchronized in FLDict
        return FLDict.getKeyString(flSharedKeys, index);
    }

    boolean dictContainsBlobs(final FLSliceResult dict) {
        // NOTE: synchronized in C4Document
        return C4Document.dictContainsBlobs(dict, flSharedKeys);
    }

    //---------------------------------------------
    // static methods
    //---------------------------------------------

    // static inline id FLValue_GetNSObject(FLValue __nullable value, cbl::SharedKeys *sk)
    public static Object valueToObject(final FLValue value, final SharedKeys sk) {
        if (sk == null) throw new RuntimeException("sk is null");
        return sk != null ? sk.valueToObject(value) : null;
    }

    // static inline NSString* FLDictIterator_GetKey(FLDictIterator *iter, cbl::SharedKeys *sk)
    public static String getKey(final FLDictIterator itr, final SharedKeys sk) {
        if (sk == null) throw new RuntimeException("sk is null");
        return sk != null && itr != null ? sk.getDictIterKey(itr) : null;
    }

    public static boolean dictContainsBlobs(final FLSliceResult dict, final SharedKeys sk) {
        if (sk == null) throw new RuntimeException("sk is null");
        return sk.dictContainsBlobs(dict);
    }
}

