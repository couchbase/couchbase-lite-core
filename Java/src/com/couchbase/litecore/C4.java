package com.couchbase.litecore;

public class C4 {
    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------

    public static native void setenv(String name, String value, int overwrite);

    public static native String getenv(String name);
}
