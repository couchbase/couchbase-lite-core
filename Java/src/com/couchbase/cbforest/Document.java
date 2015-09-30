package com.couchbase.cbforest;

public class Document {

    public String getRevID()        { return _revID; }

    public int getFlags()           { return _flags; }

    public String getType()         { return getType(_handle); }
    public void setType(String t)   { setType(_handle, t); }

    public synchronized void free() { if (_handle != 0) {free(_handle); _handle = 0;} }
    protected void finalize()       { free(); }


    public native boolean selectRevID(String revID, boolean withBody) throws ForestException;

    public native boolean selectCurrentRev();

    public native boolean selectParentRev();

    public native boolean selectNextRev();

    public native boolean selectNextLeaf(boolean includeDeleted, boolean withBody) throws ForestException;


    public String getSelectedRevID()        { return _selectedRevID; }

    public byte[] getSelectedBody() throws ForestException {
        if (_selectedBody == null) {
            _selectedBody = readSelectedBody();
        }
        return _selectedBody;
    }

    public long getSelectedSequence() { return _selectedSequence; }

    public long getSelectedRevFlags() { return _selectedRevFlags; }


    public native void insertRevision(String revID,
                                      byte[] body,
                                      boolean deleted,
                                      boolean hasAttachments,
                                      boolean allowConflict) throws ForestException;

    public native int insertRevisionWithHistory(String revID,
                                                byte[] body,
                                                boolean deleted,
                                                boolean hasAttachments,
                                                String[] history) throws ForestException;

    public native void save(int maxRevTreeDepth) throws ForestException;

    // INTERNALS:

    Document(long dbHandle, String docID, boolean mustExist) throws ForestException {
        _handle = init(dbHandle, docID, mustExist); // also sets _flags and _selectedXXX
        _docID = docID;
        _revID = _selectedRevID;
    }

    Document(long docHandle) {
        _handle = docHandle;
        _docID = initWithDocHandle(docHandle);
        _revID = _selectedRevID;
    }

    private native long init(long dbHandle, String docID, boolean mustExist) throws ForestException;
    private native String initWithDocHandle(long docHandle);

    private native static String getType(long handle);
    private native static void setType(long handle, String type);

    private native static void free(long handle);
    private native byte[] readSelectedBody() throws ForestException;

    private long _handle;
    private String _docID, _revID;
    private int _flags;

    private String _selectedRevID;
    private int _selectedRevFlags;
    private long _selectedSequence;
    private byte[] _selectedBody;
}
