package com.couchbase.litecore.fleece;

public class Fleece {
    // to call from native
    static Object MValue_toDictionary(long mv, long parent) {
        return new FleeceDict(mv, parent);
    }

    // to call from native
    static Object MValue_toArray(long mv, long parent) {
        return new FleeceArray(mv, parent);
    }

    // to call from native
    static Object toObject(long h) {
        return FLValue.toObject(new FLValue(h));
    }
}
