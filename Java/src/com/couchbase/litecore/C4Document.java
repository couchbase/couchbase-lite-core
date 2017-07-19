package com.couchbase.litecore;


public class C4Document implements C4Constants {
    //-------------------------------------------------------------------------
    // Member Variables
    //-------------------------------------------------------------------------
    private long handle = 0L; // hold pointer to C4Document

    //-------------------------------------------------------------------------
    // Constructor
    //-------------------------------------------------------------------------

    C4Document(long db, String docID, boolean mustExist) throws LiteCoreException {
        handle = get(db, docID, mustExist);
    }

    C4Document(long db, long sequence) throws LiteCoreException {
        handle = getBySequence(db, sequence);
    }

    C4Document(long handle) {
        if (handle == 0)
            throw new IllegalArgumentException("handle is 0");
        this.handle = handle;
    }

    //-------------------------------------------------------------------------
    // public methods
    //-------------------------------------------------------------------------

    // - C4Document
    public int getFlags() {
        return getFlags(handle);
    }

    public String getDocID() {
        return getDocID(handle);
    }

    public String getRevID() {
        return getRevID(handle);
    }

    public long getSequence() {
        return getSequence(handle);
    }

    // - C4Revision

    public String getSelectedRevID() {
        return getSelectedRevID(handle);
    }

    public int getSelectedFlags() {
        return getSelectedFlags(handle);
    }

    public long getSelectedSequence() {
        return getSelectedSequence(handle);
    }

    public byte[] getSelectedBody() {
        return getSelectedBody(handle);
    }

    // - Lifecycle

    public void save(int maxRevTreeDepth) throws LiteCoreException {
        save(handle, maxRevTreeDepth);
    }

    public void free() {
        if (handle != 0L) {
            free(handle);
            handle = 0L;
        }
    }

    // - Revisions

    public void selectRevision(String revID, boolean withBody) throws LiteCoreException {
        selectRevision(handle, revID, withBody);
    }

    public boolean selectCurrentRevision() {
        return selectCurrentRevision(handle);
    }

    public void loadRevisionBody() throws LiteCoreException {
        loadRevisionBody(handle);
    }

    public String detachRevisionBody() {
        return detachRevisionBody(handle);
    }

    public boolean hasRevisionBody() {
        return hasRevisionBody(handle);
    }

    public boolean selectParentRevision() {
        return selectParentRevision(handle);
    }

    public boolean selectNextRevision() {
        return selectNextRevision(handle);
    }

    public void selectNextLeafRevision(boolean includeDeleted, boolean withBody)
            throws LiteCoreException {
        selectNextLeafRevision(handle, includeDeleted, withBody);
    }

    public boolean selectFirstPossibleAncestorOf(String revID) throws LiteCoreException {
        return selectFirstPossibleAncestorOf(handle, revID);
    }

    public boolean selectNextPossibleAncestorOf(String revID) {
        return selectNextPossibleAncestorOf(handle, revID);
    }

    public boolean selectCommonAncestorRevision(String revID1, String revID2) {
        return selectCommonAncestorRevision(handle, revID1, revID2);
    }

    public boolean removeRevisionBody() {
        return removeRevisionBody(handle);
    }

    public int purgeRevision(String revID) throws LiteCoreException {
        return purgeRevision(handle, revID);
    }

    public void resolveConflict(String winningRevID, String losingRevID, byte[] mergeBody)
            throws LiteCoreException {
        resolveConflict(handle, winningRevID, losingRevID, mergeBody);
    }

    // - Creating and Updating Documents

    public C4Document update(byte[] revisionBody, int revisionFlags) throws LiteCoreException {
        return new C4Document(update(handle, revisionBody, revisionFlags));
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
        return (getFlags(handle) & flag) == flag;
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
        return (getSelectedFlags(handle) & flag) == flag;
    }

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
                                       byte[] mergeBody)
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

    static native long create(long db, String docID, byte[] body, int flags)
            throws LiteCoreException;

    static native long update(long doc, byte[] body, int flags) throws LiteCoreException;
}
