/**
 * Copyright (c) 2017 Couchbase, Inc. All rights reserved.
 * <p>
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of the License at
 * <p>
 * http://www.apache.org/licenses/LICENSE-2.0
 * <p>
 * Unless required by applicable law or agreed to in writing, software distributed under the
 * License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions
 * and limitations under the License.
 */

package com.couchbase.litecore;

import com.couchbase.litecore.fleece.FLEncoder;
import com.couchbase.litecore.fleece.FLSliceResult;

public class Database {

    //////// DATABASES:

    // Database-opening flags:
    public static final int Create = 1;
    public static final int ReadOnly = 2;
    public static final int AutoCompact = 4;
    public static final int Bundle = 8;
    public static final int SharedKeys = 0x10;
    public static final int ForestDBStorage = 0x000;
    public static final int SQLiteStorage = 0x100;

    // Encryption algorithms:
    public static final int NoEncryption = 0;
    public static final int AES256Encryption = 1;

    public Database(String path, int flags, int encryptionAlgorithm, byte[] encryptionKey) throws LiteCoreException {
        _handle = _open(path, flags, encryptionAlgorithm, encryptionKey);
    }

    public native String getPath();

    public native void close() throws LiteCoreException;

    public native void delete() throws LiteCoreException;

    public native void rekey(int encryptionAlgorithm, byte[] encryptionKey) throws LiteCoreException;

    public native void compact() throws LiteCoreException;

    public native long getDocumentCount();

    public native long getLastSequence();

    public native void beginTransaction() throws LiteCoreException;

    public native void endTransaction(boolean commit) throws LiteCoreException;

    public native boolean isInTransaction();

    public Document getDocument(String docID, boolean mustExist) throws LiteCoreException {
        return new Document(_handle, docID, mustExist);
    }

    public Document getDocumentBySequence(long sequence) throws LiteCoreException {
        return new Document(_handle, sequence);
    }

    public DocumentIterator iterator(String startDocID,
                                     String endDocID,
                                     int skip,
                                     int iteratorFlags)
            throws LiteCoreException {
        return new DocumentIterator(_handle, startDocID, endDocID, skip, iteratorFlags);
    }

    public DocumentIterator iterator(String[] docIDs, int iteratorFlags) throws LiteCoreException {
        return new DocumentIterator(_handle, docIDs, iteratorFlags);
    }

    public DocumentIterator iterateChanges(long sinceSequence, int iteratorFlags) throws LiteCoreException {
        return new DocumentIterator(_handle, sinceSequence, iteratorFlags);
    }

    protected void finalize() {
        free();
    }

    public native void free();

    public native static void deleteAtPath(String path, int flags) throws LiteCoreException;

    /**
     * Sets (or clears) a logging callback for LiteCore.
     */
    public native static void setLogger(Logger logger, int level);

    private native long _open(String path, int flags,
                              int encryptionAlgorithm, byte[] encryptionKey) throws LiteCoreException;

    long _handle; // handle to native C4Database*


    //////// DOCUMENTS

    //TODO: Review parameters with C4DocPutRequest
    /*
    https://github.com/couchbase/couchbase-lite-core/blob/7341457cdaccfd810376a4bbc58aa7a251ff26fc/C/include/c4Document.h#L227-L238
    typedef struct {
        C4String body;              ///< Revision's body
        C4String docID;             ///< Document ID
        C4String docType;           ///< Document type if any (used by indexer)
        C4RevisionFlags revFlags;   ///< Revision flags (deletion, attachments, keepBody)
        bool existingRevision;      ///< Is this an already-existing rev coming from replication?
        bool allowConflict;         ///< OK to create a conflict, i.e. can parent be non-leaf?
        const C4String *history;    ///< Array of ancestor revision IDs
        size_t historyCount;        ///< Size of history[] array
        bool save;                  ///< Save the document after inserting the revision?
        uint32_t maxRevTreeDepth;   ///< Max depth of revision tree to save (or 0 for default)
    } C4DocPutRequest;
    */
    public Document put(String docID,
                        byte[] body,
                        String docType,
                        boolean existingRevision,
                        boolean allowConflict,
                        String[] history,
                        int flags, // C4RevisionFlags
                        boolean save,
                        int maxRevTreeDepth) throws LiteCoreException {
        return new Document(_put(_handle, docID, body, docType,
                existingRevision, allowConflict, history, flags, save, maxRevTreeDepth));
    }

    public Document put(String docID,
                        FLSliceResult body, //(C4Slice*)
                        String docType,
                        boolean existingRevision,
                        boolean allowConflict,
                        String[] history,
                        int flags, // C4RevisionFlags
                        boolean save,
                        int maxRevTreeDepth) throws LiteCoreException {
        return new Document(_put(_handle, docID, body.getHandle(), docType,
                existingRevision, allowConflict, history, flags, save, maxRevTreeDepth));
    }


    public void purgeDoc(String docID) throws LiteCoreException {
        purgeDoc(_handle, docID);
    }

    private native static long _put(long dbHandle,
                                    String docID,
                                    byte[] body,
                                    String docType,
                                    boolean existingRevision,
                                    boolean allowConflict,
                                    String[] history,
                                    int flags, // C4RevisionFlags
                                    boolean save,
                                    int maxRevTreeDepth) throws LiteCoreException;

    private native static long _put(long dbHandle,
                                    String docID,
                                    long body, // C4Slice*
                                    String docType,
                                    boolean existingRevision,
                                    boolean allowConflict,
                                    String[] history,
                                    int flags, // C4RevisionFlags
                                    boolean save,
                                    int maxRevTreeDepth) throws LiteCoreException;

    private native static void purgeDoc(long dbHandle, String docID) throws LiteCoreException;

    //////// EXPIRATION

    public long expirationOfDoc(String docID) throws LiteCoreException {
        return _handle != 0 ? expirationOfDoc(_handle, docID) : 0;
    }

    public void setExpiration(String docID, long timestamp) throws LiteCoreException {
        if (_handle != 0)
            setExpiration(_handle, docID, timestamp);
    }

    public long nextDocExpiration() throws LiteCoreException {
        return _handle != 0 ? nextDocExpiration(_handle) : 0;
    }

    public String[] purgeExpiredDocuments() throws LiteCoreException {
        return _handle != 0 ? purgeExpiredDocuments(_handle) : null;
    }

    private native static long expirationOfDoc(long dbHandle, String docID) throws LiteCoreException;

    private native static void setExpiration(long dbHandle, String docID, long timestamp) throws LiteCoreException;

    private native static long nextDocExpiration(long dbHandle) throws LiteCoreException;

    private native static String[] purgeExpiredDocuments(long dbHandle) throws LiteCoreException;

    //////// RAW DOCUMENTS (i.e. info or _local)

    public void rawPut(String store, String key, byte[] meta, byte[] body) throws LiteCoreException {
        _rawPut(_handle, store, key, meta, body);
    }

    // This returns an array of two byte arrays; the first is the meta, the second is the body
    public byte[][] rawGet(String store, String key) throws LiteCoreException {
        return _rawGet(_handle, store, key);
    }

    private native static void _rawPut(long db, String store, String key, byte[] meta, byte[] body) throws LiteCoreException;

    private native static byte[][] _rawGet(long db, String store, String key) throws LiteCoreException;


    //////// BLOB STORE API:

    /**
     * Returns the BlobStore associated with a bundled database.
     * Fails if the database is not bundled.
     * NOTE: DO NOT call c4blob_freeStore on this! The C4Database will free it when it closes.
     */
    public C4BlobStore getBlobStore() throws LiteCoreException {
        return new C4BlobStore(getBlobStore(_handle));
    }

    private native static long getBlobStore(long db) throws LiteCoreException;

    ////////  INDEXES:  Defined in c4Query.h

    public boolean createIndex(String expressionsJSON, int indexType, String language, boolean ignoreDiacritics) throws LiteCoreException {
        return createIndex(_handle, expressionsJSON, indexType, language, ignoreDiacritics);
    }

    public boolean deleteIndex(String expressionsJSON, int indexType) throws LiteCoreException {
        return deleteIndex(_handle, expressionsJSON, indexType);
    }

    private native static boolean createIndex(long db, String expressionsJSON, int indexType, String language, boolean ignoreDiacritics) throws LiteCoreException;

    private native static boolean deleteIndex(long db, String expressionsJSON, int indexType) throws LiteCoreException;

    ////////  FLEECE-SPECIFIC:  Defined in c4Document+Fleece.h

    public FLEncoder createFleeceEncoder() {
        return new FLEncoder(createFleeceEncoder(_handle));
    }

    // NOTE: Should param be String instead of byte[]?
    public FLSliceResult encodeJSON(byte[] jsonData) throws LiteCoreException {
        return new FLSliceResult(encodeJSON(_handle, jsonData));
    }

    private native static long createFleeceEncoder(long db);

    private native static long encodeJSON(long db, byte[] jsonData) throws LiteCoreException;
}
