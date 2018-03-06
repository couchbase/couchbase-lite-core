//
// C4Document.java
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
package com.couchbase.litecore;


import com.couchbase.litecore.fleece.FLDict;
import com.couchbase.litecore.fleece.FLSharedKeys;
import com.couchbase.litecore.fleece.FLSliceResult;

public class C4Document implements C4Constants {
    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    private long _handle = 0L; // hold pointer to C4Document
    private boolean _managed = false; // true -> not release native object, false -> release by free()

    //-------------------------------------------------------------------------
    // Constructor
    //-------------------------------------------------------------------------

    C4Document(long db, String docID, boolean mustExist) throws LiteCoreException {
        this(get(db, docID, mustExist), false);
    }

    C4Document(long db, long sequence) throws LiteCoreException {
        this(getBySequence(db, sequence), false);
    }

    C4Document(C4Document rawDoc) {
        this(rawDoc._handle, true);
    }

    C4Document(long handle, boolean managed) {
        if (handle == 0)
            throw new IllegalArgumentException("handle is 0");
        this._handle = handle;
        this._managed = managed;
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------
    public static C4Document document(C4Document rawDoc) {
        return new C4Document(rawDoc);
    }

    public void free() {
        if (_handle != 0L && !_managed) {
            free(_handle);
            _handle = 0L;
        }
    }

    // - C4Document
    public int getFlags() {
        return getFlags(_handle);
    }

    public String getDocID() {
        return getDocID(_handle);
    }

    public String getRevID() {
        return getRevID(_handle);
    }

    public long getSequence() {
        return getSequence(_handle);
    }

    // - C4Revision

    public String getSelectedRevID() {
        return getSelectedRevID(_handle);
    }

    public int getSelectedFlags() {
        return getSelectedFlags(_handle);
    }

    public long getSelectedSequence() {
        return getSelectedSequence(_handle);
    }

    public byte[] getSelectedBody() {
        return getSelectedBody(_handle);
    }

    public FLDict getSelectedBody2() {
        long value = getSelectedBody2(_handle);
        return value == 0 ? null : new FLDict(value);
    }

    // - Lifecycle

    public void save(int maxRevTreeDepth) throws LiteCoreException {
        save(_handle, maxRevTreeDepth);
    }

    // - Revisions

    public void selectRevision(String revID, boolean withBody) throws LiteCoreException {
        selectRevision(_handle, revID, withBody);
    }

    public boolean selectCurrentRevision() {
        return selectCurrentRevision(_handle);
    }

    public void loadRevisionBody() throws LiteCoreException {
        loadRevisionBody(_handle);
    }

    public String detachRevisionBody() {
        return detachRevisionBody(_handle);
    }

    public boolean hasRevisionBody() {
        return hasRevisionBody(_handle);
    }

    public boolean selectParentRevision() {
        return selectParentRevision(_handle);
    }

    public boolean selectNextRevision() {
        return selectNextRevision(_handle);
    }

    public void selectNextLeafRevision(boolean includeDeleted, boolean withBody)
            throws LiteCoreException {
        selectNextLeafRevision(_handle, includeDeleted, withBody);
    }

    public boolean selectFirstPossibleAncestorOf(String revID) throws LiteCoreException {
        return selectFirstPossibleAncestorOf(_handle, revID);
    }

    public boolean selectNextPossibleAncestorOf(String revID) {
        return selectNextPossibleAncestorOf(_handle, revID);
    }

    public boolean selectCommonAncestorRevision(String revID1, String revID2) {
        return selectCommonAncestorRevision(_handle, revID1, revID2);
    }

    public boolean removeRevisionBody() {
        return removeRevisionBody(_handle);
    }

    public int purgeRevision(String revID) throws LiteCoreException {
        return purgeRevision(_handle, revID);
    }

    public void resolveConflict(String winningRevID, String losingRevID, byte[] mergeBody, int mergedFlags)
            throws LiteCoreException {
        resolveConflict(_handle, winningRevID, losingRevID, mergeBody, mergedFlags);
    }

    // - Creating and Updating Documents

    public C4Document update(byte[] body, int flags) throws LiteCoreException {
        return new C4Document(update(_handle, body, flags), false);
    }

    public C4Document update(FLSliceResult body, int flags) throws LiteCoreException {
        return new C4Document(update2(_handle, body != null ? body.getHandle() : 0, flags), false);
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (!(o instanceof C4Document)) return false;

        C4Document that = (C4Document) o;

        return _handle == that._handle;
    }

    @Override
    public int hashCode() {
        return (int) (_handle ^ (_handle >>> 32));
    }
    //-------------------------------------------------------------------------
    // helper methods
    //-------------------------------------------------------------------------

    // helper methods for Document
    public boolean deleted() {
        return isFlags(C4DocumentFlags.kDocDeleted);
    }

    public boolean conflicted() {
        return isFlags(C4DocumentFlags.kDocConflicted);
    }

    public boolean hasAttachments() {
        return isFlags(C4DocumentFlags.kDocHasAttachments);
    }

    public boolean exists() {
        return isFlags(C4DocumentFlags.kDocExists);
    }

    private boolean isFlags(int flag) {
        return (getFlags(_handle) & flag) == flag;
    }

    // helper methods for Revision
    public boolean selectedRevDeleted() {
        return isSelectedRevFlags(C4RevisionFlags.kRevDeleted);
    }

    public boolean selectedRevLeaf() {
        return isSelectedRevFlags(C4RevisionFlags.kRevLeaf);
    }

    public boolean selectedRevNew() {
        return isSelectedRevFlags(C4RevisionFlags.kRevNew);
    }

    public boolean selectedRevHasAttachments() {
        return isSelectedRevFlags(C4RevisionFlags.kRevHasAttachments);
    }

    private boolean isSelectedRevFlags(int flag) {
        return (getSelectedFlags(_handle) & flag) == flag;
    }

    //-------------------------------------------------------------------------
    // Fleece-related
    //-------------------------------------------------------------------------

    public static boolean hasOldMetaProperties(FLDict doc) {
        return hasOldMetaProperties(doc.getHandle());
    }

    public static byte[] encodeStrippingOldMetaProperties(FLDict doc) {
        return encodeStrippingOldMetaProperties(doc.getHandle());
    }

    // returns blobKey if the given dictionary is a [reference to a] blob; otherwise null (0)
    public static C4BlobKey dictIsBlob(FLDict dict, FLSharedKeys sk) {
        long handle = dictIsBlob(dict.getHandle(), sk == null ? 0L : sk.getHandle());
        return handle != 0 ? new C4BlobKey(handle) : null;
    }

    public static boolean dictContainsBlobs(FLSliceResult dict, FLSharedKeys sk) {
        return dictContainsBlobs2(dict.getHandle(), sk == null ? 0L : sk.getHandle());
    }

    public String bodyAsJSON(boolean canonical) throws LiteCoreException {
        return bodyAsJSON(_handle, canonical);
    }

    // doc -> pointer to C4Document

    //-------------------------------------------------------------------------
    // protected methods
    //-------------------------------------------------------------------------
    @Override
    protected void finalize() throws Throwable {
        free();
        super.finalize();
    }

    //-------------------------------------------------------------------------
    // native methods
    //-------------------------------------------------------------------------

    // - C4Document
    static native int getFlags(long doc);

    static native String getDocID(long doc);

    static native String getRevID(long doc);

    static native long getSequence(long doc);

    // - C4Revision

    static native String getSelectedRevID(long doc);

    static native int getSelectedFlags(long doc);

    static native long getSelectedSequence(long doc);

    static native byte[] getSelectedBody(long doc);

    // return pointer to FLValue
    static native long getSelectedBody2(long doc);

    // - Lifecycle

    static native long get(long db, String docID, boolean mustExist)
            throws LiteCoreException;

    static native long getBySequence(long db, long sequence) throws LiteCoreException;

    static native void save(long doc, int maxRevTreeDepth) throws LiteCoreException;

    static native void free(long doc);

    // - Revisions

    static native void selectRevision(long doc, String revID, boolean withBody)
            throws LiteCoreException;

    static native boolean selectCurrentRevision(long doc);

    static native void loadRevisionBody(long doc) throws LiteCoreException;

    static native String detachRevisionBody(long doc);

    static native boolean hasRevisionBody(long doc);

    static native boolean selectParentRevision(long doc);

    static native boolean selectNextRevision(long doc);

    static native void selectNextLeafRevision(long doc, boolean includeDeleted,
                                              boolean withBody)
            throws LiteCoreException;

    static native boolean selectFirstPossibleAncestorOf(long doc, String revID);

    static native boolean selectNextPossibleAncestorOf(long doc, String revID);

    static native boolean selectCommonAncestorRevision(long doc,
                                                       String revID1, String revID2);

    static native long getGeneration(String revID);

    static native boolean removeRevisionBody(long doc);

    static native int purgeRevision(long doc, String revID) throws LiteCoreException;

    static native void resolveConflict(long doc,
                                       String winningRevID, String losingRevID,
                                       byte[] mergeBody, int mergedFlags)
            throws LiteCoreException;

    // - Purging and Expiration

    static native void purgeDoc(long db, String docID) throws LiteCoreException;

    static native void setExpiration(long db, String docID, long timestamp)
            throws LiteCoreException;

    static native long getExpiration(long db, String docID);

    // - Creating and Updating Documents

    static native long put(long db,
                           byte[] body,
                           String docID,
                           int revFlags,
                           boolean existingRevision,
                           boolean allowConflict,
                           String[] history,
                           boolean save,
                           int maxRevTreeDepth)
            throws LiteCoreException;

    static native long put2(long db,
                            long body, // C4Slice*
                            String docID,
                            int revFlags,
                            boolean existingRevision,
                            boolean allowConflict,
                            String[] history,
                            boolean save,
                            int maxRevTreeDepth)
            throws LiteCoreException;

    static native long create(long db, String docID, byte[] body, int flags) throws LiteCoreException;

    static native long create2(long db, String docID, long body, int flags) throws LiteCoreException;

    static native long update(long doc, byte[] body, int flags) throws LiteCoreException;

    static native long update2(long doc, long body, int flags) throws LiteCoreException;

    ////////////////////////////////
    // c4Document+Fleece.h
    ////////////////////////////////

    // -- Fleece-related
    static native boolean isOldMetaProperty(String prop);

    static native boolean hasOldMetaProperties(long doc); // doc -> pointer to FLDict

    static native byte[] encodeStrippingOldMetaProperties(long doc); // doc -> pointer to FLDict

    // returns blobKey if the given dictionary is a [reference to a] blob; otherwise null (0)
    static native long dictIsBlob(long dict, long sk);

    static native boolean dictContainsBlobs(long dict, long sk); // dict -> FLDict

    static native boolean dictContainsBlobs2(long dict, long sk); // dict -> FLSliceResult

    static native String bodyAsJSON(long doc, boolean canonical) throws LiteCoreException;
    // doc -> pointer to C4Document
}
