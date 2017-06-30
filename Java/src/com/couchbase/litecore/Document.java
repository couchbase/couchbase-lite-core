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

public class Document implements Constants {

    public String getDocID() {
        return _docID;
    }

    public String getRevID() {
        return _revID;
    }

    public long getSequence() {
        return _sequence;
    }

    public int getFlags() {
        return _flags;
    }

    public synchronized void free() {
        if (_handle != 0) {
            free(_handle);
            _handle = 0;
        }
    }

    protected void finalize() {
        free();
    }

    public boolean selectRevID(String revID, boolean withBody) throws LiteCoreException {
        return selectRevID(_handle, revID, withBody);
    }

    public boolean selectCurrentRev() {
        return selectCurrentRev(_handle);
    }

    public boolean selectParentRev() {
        return selectParentRev(_handle);
    }

    public boolean selectNextRev() {
        return selectNextRev(_handle);
    }

    public boolean selectNextLeaf(boolean includeDeleted, boolean withBody) throws LiteCoreException {
        return selectNextLeaf(_handle, includeDeleted, withBody);
    }

    public boolean selectFirstPossibleAncestorOf(String revID) {
        return selectFirstPossibleAncestorOf(_handle, revID);
    }

    public boolean selectNextPossibleAncestorOf(String revID) {
        return selectNextPossibleAncestorOf(_handle, revID);
    }

    public boolean selectCommonAncestorRevision(String rev1, String rev2) {
        return selectCommonAncestorRevision(_handle, rev1, rev2);
    }

    public boolean hasRevisionBody() {
        return hasRevisionBody(_handle);
    }

    public String getSelectedRevID() {
        return _selectedRevID;
    }

    public byte[] getSelectedBody() throws LiteCoreException {
        if (_selectedBody == null) {
            _selectedBody = readSelectedBody(_handle);
        }
        return _selectedBody;
    }

    // For Test
    protected byte[] getSelectedBodyTest() {
        return _selectedBody;
    }

    public long getSelectedSequence() {
        return _selectedSequence;
    }

    public long getSelectedRevFlags() {
        return _selectedRevFlags;
    }


    public void save(int maxRevTreeDepth) throws LiteCoreException {
        save(_handle, maxRevTreeDepth);
    }

    public int purgeRevision(String revID) throws LiteCoreException {
        return purgeRevision(_handle, revID);
    }

    public boolean resolveConflict(String winningRevID,
                                   String losingRevID,
                                   byte[] mergeBody) throws LiteCoreException {
        return resolveConflict(_handle, winningRevID, losingRevID, mergeBody);
    }

    // INTERNALS:

    Document(long dbHandle, String docID, boolean mustExist) throws LiteCoreException {
        _handle = init(dbHandle, docID, mustExist); // also sets _flags and _selectedXXX
        _docID = docID;
        _revID = _selectedRevID;
    }

    Document(long dbHandle, long sequence) throws LiteCoreException {
        _handle = initWithSequence(dbHandle, sequence);
        _revID = _selectedRevID;
    }

    Document(long docHandle) {
        _handle = docHandle;
        _docID = initWithDocHandle(docHandle);
        _revID = _selectedRevID;
    }

    private native long init(long dbHandle, String docID, boolean mustExist) throws LiteCoreException;

    private native long initWithSequence(long dbHandle, long sequence) throws LiteCoreException;

    private native String initWithDocHandle(long docHandle);

    private native boolean selectRevID(long handle, String revID, boolean withBody) throws LiteCoreException;

    private native boolean selectCurrentRev(long handle);

    private native boolean selectParentRev(long handle);

    private native boolean selectNextRev(long handle);

    private native boolean selectNextLeaf(long handle, boolean includeDeleted, boolean withBody) throws LiteCoreException;

    private native boolean selectFirstPossibleAncestorOf(long handle, String revID);

    private native boolean selectNextPossibleAncestorOf(long handle, String revID);

    private native boolean selectCommonAncestorRevision(long handle, String rev1, String rev2);

    private native static boolean hasRevisionBody(long handle);

    private native static int purgeRevision(long handle, String revID) throws LiteCoreException;

    private native static boolean resolveConflict(long handle,
                                                  String winningRevID,
                                                  String losingRevID,
                                                  byte[] mergeBody) throws LiteCoreException;

    private native static void free(long handle);

    private native byte[] readSelectedBody(long handle) throws LiteCoreException;

    private native void save(long handle, int maxRevTreeDepth) throws LiteCoreException;

    protected long _handle;
    private String _docID, _revID;
    private long _sequence;
    private int _flags;

    private String _selectedRevID;
    private int _selectedRevFlags;
    private long _selectedSequence;
    private byte[] _selectedBody;

    // helper methods for Document
    public boolean deleted() {
        return isFlags(C4DocumentFlags.kDeleted);
    }

    public boolean conflicted() {
        return isFlags(C4DocumentFlags.kConflicted);
    }

    public boolean hasAttachments() {
        return isFlags(C4DocumentFlags.kHasAttachments);
    }

    public boolean exists() {
        return isFlags(C4DocumentFlags.kExists);
    }

    private boolean isFlags(int flag) {
        return (_flags & flag) == flag;
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
        return (_selectedRevFlags & flag) == flag;
    }
}
