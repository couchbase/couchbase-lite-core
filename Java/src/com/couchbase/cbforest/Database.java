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

public class Database {

    //////// DATABASES:

    // Database-opening flags:
    public static final int Create = 1;
    public static final int ReadOnly = 2;
    public static final int AutoCompact = 4;

    public static final int NoEncryption = 0;
    public static final int AES256Encryption = 1;

    public Database(String path, int flags, int encryptionAlgorithm, byte[] encryptionKey) throws ForestException {
        _handle = _open(path, flags, encryptionAlgorithm, encryptionKey);
    }

    public native void close() throws ForestException;

    public native void rekey(int encryptionAlgorithm, byte[] encryptionKey) throws ForestException;

    public native void compact() throws ForestException;

    public native long getDocumentCount();

    public native long getLastSequence();

    public native void beginTransaction() throws ForestException;

    public native void endTransaction(boolean commit) throws ForestException;

    public native boolean isInTransaction();

    public Document getDocument(String docID, boolean mustExist) throws ForestException {
        return new Document(_handle, docID, mustExist);
    }

    public Document getDocumentBySequence(long sequence) throws ForestException {
        return new Document(_handle, sequence);
    }

    public DocumentIterator iterator(String startDocID,
                                     String endDocID,
                                     int skip,
                                     int iteratorFlags)
            throws ForestException {
        return new DocumentIterator(_handle, startDocID, endDocID, skip, iteratorFlags);
    }

    public DocumentIterator iterator(String[] docIDs, int iteratorFlags) throws ForestException {
        return new DocumentIterator(_handle, docIDs, iteratorFlags);
    }

    public DocumentIterator iterateChanges(long sinceSequence, int iteratorFlags) throws ForestException {
        return new DocumentIterator(_handle, sinceSequence, iteratorFlags);
    }

    protected void finalize() {
        free();
    }

    private native void free();

    /** Sets (or clears) a logging callback for CBForest. */
    public native static void setLogger(Logger logger, int level);

    private native long _open(String path, int flags,
                              int encryptionAlgorithm, byte[] encryptionKey) throws ForestException;

    long _handle; // handle to native C4Database*


    //////// DOCUMENTS

    public void purgeDoc(String docID) throws ForestException {
        purgeDoc(_handle, docID);
    }

    private native static void purgeDoc(long dbHandle, String docID) throws ForestException;

    //////// EXPIRATION

    public long expirationOfDoc(String docID) throws ForestException {
        return _handle != 0 ? expirationOfDoc(_handle, docID) : 0;
    }

    public void setExpiration(String docID, long timestamp) throws ForestException {
        if (_handle != 0)
            setExpiration(_handle, docID, timestamp);
    }

    public long nextDocExpiration() throws ForestException {
        return _handle != 0 ? nextDocExpiration(_handle) : 0;
    }

    public String[] purgeExpiredDocuments() throws ForestException {
        return _handle != 0 ? purgeExpiredDocuments(_handle) : null;
    }

    private native static long expirationOfDoc(long dbHandle, String docID) throws ForestException;

    private native static void setExpiration(long dbHandle, String docID, long timestamp) throws ForestException;

    private native static long nextDocExpiration(long dbHandle) throws ForestException;

    private native static String[] purgeExpiredDocuments(long dbHandle) throws ForestException;

    //////// RAW DOCUMENTS (i.e. info or _local)

    public void rawPut(String store, String key, byte[] meta, byte[] body) throws ForestException {
        _rawPut(_handle, store, key, meta, body);
    }

    // This returns an array of two byte arrays; the first is the meta, the second is the body
    public byte[][] rawGet(String store, String key) throws ForestException {
        return _rawGet(_handle, store, key);
    }

    private native static void _rawPut(long db, String store, String key, byte[] meta, byte[] body) throws ForestException;
    private native static byte[][] _rawGet(long db, String store, String key) throws ForestException;

}
