//
//  JSONEndpoint.cc
//  LiteCore
//
//  Created by Jens Alfke on 8/19/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "JSONEndpoint.hh"


void JSONEndpoint::prepare(bool readOnly, bool mustExist, slice docIDProperty) {
    Endpoint::prepare(readOnly, mustExist, docIDProperty);
    if (!docIDProperty)
        _docIDProperty = c4str("_id");
    bool err;
    if (readOnly) {
        _in.reset(new ifstream(_spec, ios_base::in));
        err = _in->fail();
        if (!err && _in->peek() != '{')
            fail("Source file does not appear to contain JSON objects (does not start with '{').");
    } else {
        if (mustExist && remove(_spec.c_str()) != 0)
            fail(format("Destination JSON file %s doesn't exist or is not writeable [--existing]",
                        _spec.c_str()));
        _out.reset(new ofstream(_spec, ios_base::trunc | ios_base::out));
        err = _out->fail();
    }
    if (err)
        fail(format("Couldn't open JSON file %s", _spec.c_str()));
}


// As source:
void JSONEndpoint::copyTo(Endpoint *dst, uint64_t limit) {
    string line;
    unsigned lineNo;
    for (lineNo = 0; lineNo < limit && getline(*_in, line); ++lineNo) {
        dst->writeJSON(nullslice, c4str(line));
    }
    if (_in->bad())
        errorOccurred("Couldn't read JSON file");
    else if (lineNo == limit)
        cout << "Stopped after " << limit << " documents.\n";
}


// As destination:
void JSONEndpoint::writeJSON(slice docID, slice json) {
    if (docID) {
        *_out << "{\"" << to_string(_docIDProperty) << "\":\"";
        _out->write((char*)docID.buf, docID.size);
        *_out << "\",";
        json.moveStart(1);
    }
    _out->write((char*)json.buf, json.size);
    *_out << '\n';
    logDocument(docID);
}
