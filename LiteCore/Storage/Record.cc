//
// Record.cc
//
// Copyright (c) 2014 Couchbase, Inc All rights reserved.
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

#include "Record.hh"
#include "Endian.hh"

using namespace std;

namespace litecore {

    Record::Record(slice key)
    :Record()
    {
        setKey(key);
    }

    Record::Record(const Record &d)
    :_key(d._key),
     _version(d._version),
     _body(d._body),
     _bodySize(d._bodySize),
     _sequence(d._sequence),
     _flags(d._flags),
     _exists(d._exists)
    { }

    Record::Record(Record &&d) noexcept
    :_key(move(d._key)),
     _version(move(d._version)),
     _body(move(d._body)),
     _bodySize(d._bodySize),
     _sequence(d._sequence),
     _flags(d._flags),
     _exists(d._exists)
    { }

    void Record::clearMetaAndBody() noexcept {
        setVersion(nullslice);
        setBody(nullslice);
        _bodySize = _sequence = 0;
        _flags = DocumentFlags::kNone;
        _exists = false;
    }

    void Record::clear() noexcept {
        clearMetaAndBody();
        setKey(nullslice);
    }

    uint64_t Record::bodyAsUInt() const noexcept {
        uint64_t count;
        if (body().size < sizeof(count))
            return 0;
        memcpy(&count, body().buf, sizeof(count));
        return _endian_decode(count);
    }

    void Record::setBodyAsUInt(uint64_t n) noexcept {
        uint64_t newBody = _endian_encode(n);
        setBody(slice(&newBody, sizeof(newBody)));
    }



}
