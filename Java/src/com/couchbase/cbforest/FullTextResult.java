package com.couchbase.cbforest;

public class FullTextResult {

    FullTextResult(View view, String docID, long sequence, int fullTextID, int[] terms) {
        _view = view;
        _docID = docID;
        _sequence = sequence;
        _fullTextID = fullTextID;
        _terms = terms;
    }

    public int getMatchCount() {
        return _terms.length / 3;
    }

    public int getTermNumber(int matchNumber) {
        return _terms[matchNumber / 3];
    }

    public int getByteOffset(int matchNumber) {
        return _terms[matchNumber / 3 + 1];
    }

    public int getByteLength(int matchNumber) {
        return _terms[matchNumber / 3 + 2];
    }

    public String getFullText() throws ForestException {
        return getFullText(_view._handle, _docID, _sequence, _fullTextID);
    }

    private static native String getFullText(long viewHandle, String docID, long sequence,
                                             int fullTextID) throws ForestException;

    private final View _view;
    private final String _docID;
    private final long _sequence;
    private final int _fullTextID;
    private final int[] _terms;
}
