package com.couchbase.cbforest;

public class QueryIterator {

    QueryIterator(View view, long handle) {
        _view = view;
        _handle = handle;
    }

    public boolean next() throws ForestException    {return next(_handle);}

    public byte[] keyJSON()                         {return keyJSON(_handle);}
    public byte[] valueJSON()                       {return valueJSON(_handle);}
    public String docID()                           {return docID(_handle);}
    public long   sequence()                        {return sequence(_handle);}

    public FullTextResult fullTextResult() {
        return new FullTextResult(_view, docID(), sequence(),
                                  fullTextID(_handle), fullTextTerms(_handle));
    }

    /** Returns the bounding box of a geo-query match as an array of coordinates,
        in the order (xmin, ymin, xmax, ymax). */
    public double[] geoBoundingBox()                {return geoBoundingBox(_handle);}

    /** Returns the GeoJSON of a geo-query match, exactly as it was emitted from the map function. */
    public byte[] geoJSON()                         {return geoJSON(_handle);}

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
    private native int fullTextID(long handle);
    private native int[] fullTextTerms(long handle);
    private native double[] geoBoundingBox(long handle);
    private native byte[] geoJSON(long handle);
    private native void free(long handle);

    private View _view;
    private long _handle;  // Handle to native C4QueryEnumerator*
}
