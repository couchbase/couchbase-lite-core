//
// Endpoint.hh
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

#pragma once
#include "Tool.hh"
#include <memory>

using namespace fleeceapi;


/** Abstract base class for a source or target of copying/replication. */
class Endpoint {
public:
    Endpoint(const string &spec)
    :_spec(spec)
    { }

    static Endpoint* create(const string &str);
    static Endpoint* create(C4Database*);
    virtual ~Endpoint() { }

    virtual bool isDatabase() const {
        return false;
    }

    virtual void prepare(bool isSource, bool mustExist, slice docIDProperty, const Endpoint *other) {
        if (docIDProperty.size > 0)
            _docIDProperty = docIDProperty;
        else
            _docIDProperty = "_id"_sl;
    }

    virtual void copyTo(Endpoint*, uint64_t limit) =0;

    virtual void writeJSON(slice docID, slice json) = 0;

    virtual void finish() { }

    uint64_t docCount()             {return _docCount;}
    void setDocCount(uint64_t n)    {_docCount = n;}

    void logDocument(slice docID) {
        ++_docCount;
        if (Tool::instance->verbose() >= 2)
            cout << to_string(docID) << '\n';
        else if (Tool::instance->verbose() == 1 && (_docCount % 1000) == 0)
            cout << _docCount << '\n';
    }

    void logDocuments(unsigned n) {
        _docCount += n;
        if (Tool::instance->verbose() >= 2)
            cout << n << " more documents\n";
        else if (Tool::instance->verbose() == 1 && (_docCount % 1000) < n)
            cout << _docCount << '\n';
    }

protected:

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
                Tool::instance->fail(format("Property \"%.*s\" is not a scalar in JSON: %.*s", SPLAT(_docIDProperty), SPLAT(json)));
        } else {
            Tool::instance->errorOccurred(format("No property \"%.*s\" in JSON: %.*s", SPLAT(_docIDProperty), SPLAT(json)));
        }
        return docIDBuf;
    }

    const string _spec;
    Encoder _encoder;
    alloc_slice _docIDProperty;
    uint64_t _docCount {0};
    unique_ptr<KeyPath> _docIDPath;
};


