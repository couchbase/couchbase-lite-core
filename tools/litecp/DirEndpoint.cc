//
//  DirEndpoint.cc
//  LiteCore
//
//  Created by Jens Alfke on 8/19/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "DirEndpoint.hh"


void DirectoryEndpoint::prepare(bool readOnly, bool mustExist, slice docIDProperty, const Endpoint *other) {
    Endpoint::prepare(readOnly, mustExist, docIDProperty, other);
    if (_dir.exists()) {
        if (!_dir.existsAsDir())
            fail(format("%s is not a directory", _spec.c_str()));
    } else {
        if (readOnly || mustExist)
            fail(format("Directory %s doesn't exist", _spec.c_str()));
        else
            _dir.mkdir();
    }
    if (docIDProperty) {
        _docIDPath.reset(new KeyPath(docIDProperty, nullptr, nullptr));
        if (!*_docIDPath)
            fail("Invalid key-path");
    }
}


// As source:
void DirectoryEndpoint::copyTo(Endpoint *dst, uint64_t limit) {
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
            errorOccurred(format("No doc ID for JSON: %.*s", SPLAT(json)));
        if (!docID)
            return;
    }

    if (docID.size == 0 || docID[0] == '.' || docID.findByte(FilePath::kSeparator[0])) {
        errorOccurred(format("writing doc \"%.*s\": doc ID cannot be used as a filename", SPLAT(docID)));
        return;
    }

    FilePath jsonFile = _dir[docID.asString() + ".json"];
    ofstream out(jsonFile.path(), ios_base::trunc | ios_base::out);
    out.write((char*)json.buf, json.size);
    out << '\n';
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
        errorOccurred(format("reading file %s", path.c_str()));
        return nullslice;
    }
    return {buffer.buf, readBytes};
}
