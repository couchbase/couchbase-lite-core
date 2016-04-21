//  Copyright © 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

package com.couchbase.cbforest;

public class QueryIterator {

    QueryIterator(View view, long handle) {
        _view = view;
        _handle = handle;
    }

    public boolean next() throws ForestException {
        boolean ok = next(_handle);
        if (!ok)
            _handle = 0;
        return ok;
    }

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

    protected void finalize() {
        if (_handle != 0)
            free(_handle);
    }

    private static native boolean next(long handle) throws ForestException;
    private static native byte[] keyJSON(long handle);
    private static native byte[] valueJSON(long handle);
    private static native String docID(long handle);
    private static native long sequence(long handle);
    private static native int fullTextID(long handle);
    private static native int[] fullTextTerms(long handle);
    private static native double[] geoBoundingBox(long handle);
    private static native byte[] geoJSON(long handle);
    private static native void free(long handle);

    private View _view;
    private long _handle;  // Handle to native C4QueryEnumerator*
}
