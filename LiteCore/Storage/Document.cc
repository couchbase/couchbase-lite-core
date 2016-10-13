//
//  Document.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 11/11/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Document.hh"
#include "forestdb_endian.h"

using namespace std;

namespace litecore {

    Document::Document(slice key)
    :Document()
    {
        setKey(key);
    }

    Document::Document(const Document &d)
    :_key(d._key),
     _meta(d._meta),
     _body(d._body),
     _bodySize(d._bodySize),
     _sequence(d._sequence),
     _offset(d._offset),
     _deleted(d._deleted),
     _exists(d._exists)
    { }

    Document::Document(Document &&d) noexcept
    :_key(move(d._key)),
     _meta(move(d._meta)),
     _body(move(d._body)),
     _bodySize(d._bodySize),
     _sequence(d._sequence),
     _offset(d._offset),
     _deleted(d._deleted),
     _exists(d._exists)
    { }

    void Document::clearMetaAndBody() noexcept {
        setMeta(nullslice);
        setBody(nullslice);
        _bodySize = _sequence = _offset = 0;
        _exists = _deleted = false;
    }

    void Document::clear() noexcept {
        clearMetaAndBody();
        setKey(nullslice);
    }

    uint64_t Document::bodyAsUInt() const noexcept {
        uint64_t count;
        if (body().size < sizeof(count))
            return 0;
        memcpy(&count, body().buf, sizeof(count));
        return _endian_decode(count);
    }

    void Document::setBodyAsUInt(uint64_t n) noexcept {
        uint64_t newBody = _endian_encode(n);
        setBody(slice(&newBody, sizeof(newBody)));
    }



}
