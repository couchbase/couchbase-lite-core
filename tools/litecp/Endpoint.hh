//
//  Endpoint.hh
//  LiteCore
//
//  Created by Jens Alfke on 8/19/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "ToolUtils.hh"
#include <memory>


/** Abstract base class for a source or target of copying/replication. */
class Endpoint {
public:
    Endpoint(const string &spec)
    :_spec(spec)
    { }

    static Endpoint* create(const string &str);
    virtual ~Endpoint() { }
    virtual void prepare(bool readOnly, bool mustExist, slice docIDProperty) {
        _docIDProperty = docIDProperty;
    }
    virtual void copyTo(Endpoint*, uint64_t limit) =0;

    virtual void writeJSON(slice docID, slice json) = 0;

    virtual void finish() { }

    uint64_t docCount() {
        return _docCount;
    }

protected:

    void logDocument(slice docID) {
        ++_docCount;
        if (gVerbose >= 2)
            cout << to_string(docID) << '\n';
        else if (gVerbose == 1 && (_docCount % 1000) == 0)
            cout << _docCount << '\n';
    }

    alloc_slice docIDFromJSON(slice json) {
        alloc_slice body = Encoder::convertJSON(json, nullptr);
        return docIDFromFleece(body, json);
    }

    alloc_slice docIDFromFleece(slice body, slice json) {
        alloc_slice docIDBuf;
        Dict root = Value::fromTrustedData(body).asDict();
        Value docIDProp = root[*_docIDPath];
        if (docIDProp) {
            docIDBuf = docIDProp.toString();
            if (!docIDBuf)
                fail(format("Property \"%.*s\" is not a scalar in JSON: %.*s", SPLAT(_docIDProperty), SPLAT(json)));
        } else {
            errorOccurred(format("No property \"%.*s\" in JSON: %.*s", SPLAT(_docIDProperty), SPLAT(json)));
        }
        return docIDBuf;
    }

    const string _spec;
    Encoder _encoder;
    alloc_slice _docIDProperty;
    uint64_t _docCount {0};
    unique_ptr<KeyPath> _docIDPath;
};


