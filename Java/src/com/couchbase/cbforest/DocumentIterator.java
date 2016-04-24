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

public class DocumentIterator {
    DocumentIterator(long handle, boolean dummy) {
        _handle = handle;
        _currentIDs = new String[2];
        _currentNumbers = new long[2];
    }

    DocumentIterator(long dbHandle,
                     String startDocID,
                     String endDocID,
                     int skip,
                     int iteratorFlags)
            throws ForestException {
        this(initEnumerateAllDocs(dbHandle, startDocID, endDocID, skip, iteratorFlags), false);
    }

    DocumentIterator(long dbHandle, String[] docIDs, int iteratorFlags) throws ForestException {
        this(initEnumerateSomeDocs(dbHandle, docIDs, iteratorFlags), false);
    }

    DocumentIterator(long dbHandle, long sinceSequence, int iteratorFlags) throws ForestException {
        this(initEnumerateChanges(dbHandle, sinceSequence, iteratorFlags), false);
    }

    // Advances iterator. Then call getCurrentDocID(), etc. to get attributes of document. */
    public boolean next() throws ForestException {
        _hasCurrentInfo = false;
        if (next(_handle))
            return true;
        _handle = 0; // native iterator is already freed
        return false;
    }

    public String getCurrentDocID()  {getCurrentInfo(); return _currentIDs[0];}
    public String getCurrentRevID()  {getCurrentInfo(); return _currentIDs[1];}
    public int getCurrentDocFlags()  {getCurrentInfo(); return (int)_currentNumbers[0];}
    public long getCurrentSequence() {getCurrentInfo(); return _currentNumbers[1];}

    // Returns current Document
    public Document getDocument() throws ForestException {
        return new Document(getDocumentHandle(_handle));
    }

    // Returns next Document, or null at end
    public Document nextDocument() throws ForestException {
        return next() ? getDocument() : null;
    }

    protected void finalize()       { if (_handle != 0) free(_handle); }

    private void getCurrentInfo() {
        if (!_hasCurrentInfo) {
            getDocumentInfo(_handle, _currentIDs, _currentNumbers);
            _hasCurrentInfo = true;
        }
    }

    private static native long initEnumerateChanges(long dbHandle, long sinceSequence, int optionFlags) throws ForestException;
    private static native long initEnumerateAllDocs(long dbHandle,
                                             String startDocID,
                                             String endDocID,
                                             int skip,
                                             int optionFlags)
            throws ForestException;
    private static native long initEnumerateSomeDocs(long dbHandle, String[] docIDs, int optionFlags) throws ForestException;
    private native static boolean next(long handle) throws ForestException;
    private native static long getDocumentHandle(long handle) throws ForestException;
    private native static void getDocumentInfo(long handle, Object[] outIDs, long[] outNumbers);
    private native static void free(long handle);

    private long _handle;
    private boolean _hasCurrentInfo;
    private String[] _currentIDs;
    private long[] _currentNumbers;
}
