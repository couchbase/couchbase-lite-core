package com.couchbase.litecore;

import java.util.Arrays;
import java.util.List;

public class C4FullTextMatch {
    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    long _handle = 0L; // hold pointer to C4FullTextMatch

    //-------------------------------------------------------------------------
    // Constructor
    //-------------------------------------------------------------------------
    C4FullTextMatch(long handle) {
        this._handle = handle;
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public long dataSource() {
        return dataSource(_handle);
    }

    public long property() {
        return property(_handle);
    }

    public long term() {
        return term(_handle);
    }

    public long start() {
        return start(_handle);
    }

    public long length() {
        return length(_handle);
    }

    public List<Long> toList() {
        return Arrays.asList(dataSource(), property(), term(), start(), length());
    }

    //-------------------------------------------------------------------------
    // protected methods
    //-------------------------------------------------------------------------
    @Override
    protected void finalize() throws Throwable {
        super.finalize();
    }

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------

    static native long dataSource(long handle);

    static native long property(long handle);

    static native long term(long handle);

    static native long start(long handle);

    static native long length(long handle);
}
