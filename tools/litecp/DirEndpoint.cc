//
// DirEndpoint.cc
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

#include "DirEndpoint.hh"


void DirectoryEndpoint::prepare(bool isSource, bool mustExist, slice docIDProperty, const Endpoint *other) {
    Endpoint::prepare(isSource, mustExist, docIDProperty, other);
    if (_dir.exists()) {
        if (!_dir.existsAsDir())
            Tool::instance->fail(format("%s is not a directory", _spec.c_str()));
    } else {
        if (isSource || mustExist)
            Tool::instance->fail(format("Directory %s doesn't exist", _spec.c_str()));
        else
            _dir.mkdir();
    }
    if (docIDProperty) {
        _docIDPath.reset(new KeyPath(docIDProperty, nullptr, nullptr));
        if (!*_docIDPath)
            Tool::instance->fail("Invalid key-path");
    }
}


// As source:
void DirectoryEndpoint::copyTo(Endpoint *dst, uint64_t limit) {
    if (Tool::instance->verbose())
        cout << "Importing JSON files...\n";
    alloc_slice buffer(10000);
    _dir.forEachFile([&](const FilePath &file) {
        string filename = file.fileName();
        if (!hasSuffix(filename, ".json") || hasPrefix(filename, "."))
            return;
        string docID = filename.substr(0, filename.size() - 5);

        slice json = readFile(file.path(), buffer);
        if (json)
            dst->writeJSON(docID, json);
    });
}


// As destination:
void DirectoryEndpoint::writeJSON(slice docID, slice json) {
    alloc_slice docIDBuf;
    if (!docID) {
        if (_docIDProperty)
            docID = docIDBuf = docIDFromJSON(json);
        else
            Tool::instance->errorOccurred(format("No doc ID for JSON: %.*s", SPLAT(json)));
        if (!docID)
            return;
    }

    if (docID.size == 0 || docID[0] == '.' || docID.findByte(FilePath::kSeparator[0])) {
        Tool::instance->errorOccurred(format("writing doc \"%.*s\": doc ID cannot be used as a filename", SPLAT(docID)));
        return;
    }

    FilePath jsonFile = _dir[docID.asString() + ".json"];
    ofstream out(jsonFile.path(), ios_base::trunc | ios_base::out);
    out << json << '\n';
    logDocument(docID);
}


slice DirectoryEndpoint::readFile(const string &path, alloc_slice &buffer) {
    size_t readBytes = 0;
    ifstream in(path, ios_base::in);
    do {
        if (readBytes == buffer.size)
            buffer.resize(2*buffer.size);
        in.read((char*)buffer.buf + readBytes, buffer.size - readBytes);
        readBytes += in.gcount();
    } while (in.good());
    if (in.bad()) {
        Tool::instance->errorOccurred(format("reading file %s", path.c_str()));
        return nullslice;
    }
    return {buffer.buf, readBytes};
}
