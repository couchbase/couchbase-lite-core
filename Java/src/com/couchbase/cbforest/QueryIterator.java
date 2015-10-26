package com.couchbase.cbforest;

public class QueryIterator {

    QueryIterator(long handle) {
        _handle = handle;
    }

    public boolean next() throws ForestException    {return next(_handle);}

    public byte[] keyJSON()                         {return keyJSON(_handle);}
    public byte[] valueJSON()                       {return valueJSON(_handle);}
    public String docID()                           {return docID(_handle);}
    public long   sequence()                        {return sequence(_handle);}

    public void free() {
        free(_handle);
    }

    protected void finalize() {
        free();
    }

    private native boolean next(long handle) throws ForestException;
    private native byte[] keyJSON(long handle);
    private native byte[] valueJSON(long handle);
    private native String docID(long handle);
    private native long sequence(long handle);
    private native void free(long handle);

    private long _handle;  // Handle to native C4QueryEnumerator*
}
