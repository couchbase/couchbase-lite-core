package com.couchbase.litecore;

public class C4Key {
    public static native byte[] derivePBKDF2SHA256Key(String password, byte[] salt, int rounds);
}
