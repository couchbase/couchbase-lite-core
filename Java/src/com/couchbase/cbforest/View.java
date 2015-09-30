package com.couchbase.cbforest;

import java.util.List;
import java.util.Map;


public class View {

    public View(Database sourceDB, String viewDbPath, int viewDbFlags, String viewName, String version) throws ForestException {
        _handle = _open(sourceDB._handle, viewDbPath, viewDbFlags, viewName, version);
    }

    public native void close();

    public native void eraseIndex() throws ForestException;

    public native void delete() throws ForestException;


    public native long getTotalRows();

    public native long getLastSequenceIndexed();

    public native long getLastSequenceChangedAt();


    public QueryIterator query(long skip,
                               long limit,
                               boolean descending,
                               boolean inclusiveStart,
                               boolean inclusiveEnd,
                               Object startKey,
                               Object endKey,
                               String startKeyDocID,
                               String endKeyDocID) throws ForestException
    {
        return new QueryIterator(query(_handle, skip, limit, descending,
                                       inclusiveStart, inclusiveEnd,
                                       objectToKey(startKey), objectToKey(endKey),
                                       startKeyDocID, endKeyDocID));
    }


    public QueryIterator query(long skip,
                               long limit,
                               boolean descending,
                               boolean inclusiveStart,
                               boolean inclusiveEnd,
                               Object keys[]) throws ForestException
    {
        long keyHandles[] = new long[keys.length];
        int i = 0;
        for (Object key : keys) {
            keyHandles[i++] = objectToKey(key);
        }
        return new QueryIterator(query(_handle, skip, limit, descending,
                                       inclusiveStart, inclusiveEnd, keyHandles));
    }


    // Key generation:

    static long objectToKey(Object o) {
        if (o == null) {
            return 0;
        } else {
            long key = newKey();
            try {
                keyAdd(key, o);
                return key;
            } catch (Throwable t) {
                freeKey(key);
                throw t; //? Is this correct?
            }
        }
    }

    private static void keyAdd(long key, Object o) {
        if (o == null) {
            keyAddNull(key);
        } else if (o instanceof Boolean) {
            keyAdd(key, ((Boolean)o).booleanValue());
        } else if (o instanceof Number) {
            keyAdd(key, ((Number)o).doubleValue());
        } else if (o instanceof String) {
            keyAdd(key, (String)o);
        } else if (o instanceof Object[]) {
            Object[] array = (Object[])o;
            for (Object item : (Object[])o) {
                keyAdd(key, item);
            }
            keyEndArray(key);
        } else if (o instanceof List) {
            keyBeginArray(key);
            for (Object item : (List)o) {
                keyAdd(key, item);
            }
            keyEndArray(key);
        } else if (o instanceof Map) {
            keyBeginMap(key);
            //FIX: The next line gives a warning about an unchecked cast. How to fix it?
            for (Map.Entry entry : ((Map<String,Object>)o).entrySet()) {
                keyAdd(key, (String)entry.getKey());
                keyAdd(key, entry.getValue());
            }
            keyEndMap(key);
        } else {
            throw new Error("invalid class for JSON"); //FIX: What's the correct error class?
        }
    }

    static native long newKey();
    static native void freeKey(long key);
    static native void keyAddNull(long key);
    static native void keyAdd(long key, boolean b);
    static native void keyAdd(long key, double d);
    static native void keyAdd(long key, String s);
    static native void keyBeginArray(long key);
    static native void keyEndArray(long key);
    static native void keyBeginMap(long key);
    static native void keyEndMap(long key);


    // Internals:
    
    protected void finalize() {
        close();
    }

    private static native long query(long viewHandle,   // C4View*
                                     long skip,
                                     long limit,
                                     boolean descending,
                                     boolean inclusiveStart,
                                     boolean inclusiveEnd,
                                     long startKey,     // C4Key*
                                     long endKey,       // C4Key*
                                     String startKeyDocID,
                                     String endKeyDocID) throws ForestException;

    private static native long query(long viewHandle,   // C4View*
                                     long skip,
                                     long limit,
                                     boolean descending,
                                     boolean inclusiveStart,
                                     boolean inclusiveEnd,
                                     long keys[]) throws ForestException; // array of C4Key*

    private native long _open(long sourceDbHandle, String viewDbPath, int viewDbFlags, String viewName, String version) throws ForestException;

    private long _handle; // handle to native C4View*
}
