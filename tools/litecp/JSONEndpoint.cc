//
// JSONEndpoint.cc
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

#include "JSONEndpoint.hh"


void JSONEndpoint::prepare(bool isSource, bool mustExist, slice docIDProperty, const Endpoint *other) {
    Endpoint::prepare(isSource, mustExist, docIDProperty, other);
    bool err;
    if (isSource) {
        _in.reset(new ifstream(_spec, ios_base::in));
        err = _in->fail();
        if (!err && _in->peek() != '{')
            Tool::instance->fail("Source file does not appear to contain JSON objects (does not start with '{').");
    } else {
        if (mustExist && remove(_spec.c_str()) != 0)
            Tool::instance->fail(format("Destination JSON file %s doesn't exist or is not writeable [--existing]",
                        _spec.c_str()));
        _out.reset(new ofstream(_spec, ios_base::trunc | ios_base::out));
        err = _out->fail();
    }
    if (err)
        Tool::instance->fail(format("Couldn't open JSON file %s", _spec.c_str()));
}


// As source:
void JSONEndpoint::copyTo(Endpoint *dst, uint64_t limit) {
    if (Tool::instance->verbose())
        cout << "Importing JSON file...\n";
    string line;
    unsigned lineNo;
    for (lineNo = 0; lineNo < limit && getline(*_in, line); ++lineNo) {
        dst->writeJSON(nullslice, c4str(line));
    }
    if (_in->bad())
        Tool::instance->errorOccurred("Couldn't read JSON file");
    else if (lineNo == limit)
        cout << "Stopped after " << limit << " documents.\n";
}


// As destination:
void JSONEndpoint::writeJSON(slice docID, slice json) {
    if (docID) {
        *_out << "{\"" << _docIDProperty << "\":\"" << docID << "\",";
        json.moveStart(1);
    }
    *_out << json << '\n';
    logDocument(docID);
}
