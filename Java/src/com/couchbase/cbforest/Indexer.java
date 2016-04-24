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


public class Indexer {

    public Indexer(View[] views) throws ForestException {
        assert(views.length > 0);
        _views = views;
        Database database = views[0].database();
        long viewHandles[] = new long[views.length];
        for (int i = 0; i < views.length; i++) {
            assert(views[i].database() == database);
            viewHandles[i] = views[i]._handle;
        }
        _handle = beginIndex(database._handle, viewHandles);
    }

    public void triggerOnView(View v) {
        triggerOnView(_handle, v._handle);
    }

    public DocumentIterator iterateDocuments() throws ForestException {
        long iterHandle = iterateDocuments(_handle);
        if (iterHandle == 0)
            return null;  // means there's nothing new to iterate
        return new DocumentIterator(iterHandle, false);
    }

    public boolean shouldIndex(Document doc, int viewNumber) {
        return shouldIndex(_handle, doc._handle, viewNumber);
    }

    public void emit(Document doc, int viewNumber, Object[] keys, byte[][] values) throws ForestException {

        // initialize C4Key
        long keyHandles[] = new long[keys.length];
        for (int i = 0; i < keys.length; i++) {
            keyHandles[i] = View.objectToKey(keys[i]);
        }

        emit(_handle, doc._handle, viewNumber, keyHandles, values);
        // C4Keys in keyHandles are freed by the native method
    }

    public void endIndex(boolean commit) throws ForestException {
        endIndex(_handle, commit);
        _handle = 0;
    }


    // internals:

    protected void finalize() {
        if (_handle != 0) {
            try {
                endIndex(false);
            } catch (Exception x) { }
        }
    }

    private static native long beginIndex(long dbHandle, long viewHandles[]) throws ForestException;
    private static native void triggerOnView(long handle, long viewHandle);
    private static native long iterateDocuments(long handle) throws ForestException;
    private static native boolean shouldIndex(long handle, long docHandle, int viewNumber);
    private static native void emit(long handle, long docHandle, int viewNumber, long[] keys, byte[][] values) throws ForestException;
    private static native void endIndex(long handle, boolean commit) throws ForestException;

    private long _handle;   // handle to native C4Indexer*
    private View[] _views;  // Must keep the View objects from being GC'd

}
