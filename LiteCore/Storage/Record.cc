//
// Record.cc
//
// Copyright 2014-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include <utility>

#include "Record.hh"
#include "Endian.hh"

using namespace std;
using namespace fleece;

namespace litecore {

    Record::Record(slice key) : _key(key) {}

    Record::Record(alloc_slice key) : _key(std::move(key)) {}

    void Record::clear() noexcept {
        _key = _version = _body = _extra = nullslice;
        _bodySize = _extraSize = _subsequence = 0;
        _sequence                             = 0_seq;
        _flags                                = DocumentFlags::kNone;
        _contentLoaded                        = kMetaOnly;
        _exists                               = false;
    }

    uint64_t Record::bodyAsUInt() const noexcept {
        uint64_t count;
        if ( body().size < sizeof(count) ) return 0;
        memcpy(&count, body().buf, sizeof(count));
        return fleece::endian::dec64(count);
    }

    void Record::setBodyAsUInt(uint64_t n) noexcept {
        uint64_t newBody = fleece::endian::enc64(n);
        setBody(slice(&newBody, sizeof(newBody)));
    }

    RecordUpdate::RecordUpdate(slice key_, slice body_, DocumentFlags flags_)
        : key(std::move(key_)), body(std::move(body_)), flags(flags_) {}

    RecordUpdate::RecordUpdate(const Record& rec) : RecordUpdate(rec.key(), rec.body(), rec.flags()) {
        version     = rec.version();
        extra       = rec.extra();
        sequence    = rec.sequence();
        subsequence = rec.subsequence();
    }

}  // namespace litecore
