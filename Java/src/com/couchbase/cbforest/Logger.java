package com.couchbase.cbforest;

public interface Logger {
    public static final int Debug = 0, Info = 1, Warning = 2, Error = 3, None = 4;
    
    public void log(int level, String message);
}
