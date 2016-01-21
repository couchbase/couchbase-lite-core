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
