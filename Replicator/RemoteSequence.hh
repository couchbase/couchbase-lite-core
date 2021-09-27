//
// RemoteSequence.hh
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "StringUtil.hh"
#include "fleece/Fleece.hh"
#include "slice_stream.hh"
#include <variant>
#include <cinttypes>

namespace litecore::repl {

    /** A sequence received from a remote peer. Can be any JSON value, but optimized for positive ints. */
    class RemoteSequence {
    public:
        RemoteSequence() noexcept
        :_value(fleece::nullslice)
        { }

        explicit RemoteSequence(fleece::Value val) {
            if (val.isInteger())
                _value = val.asUnsigned();
            else
                _value = val.toJSON();
        }

        explicit RemoteSequence(fleece::slice json) {
            if (json.size == 0) {
                _value = fleece::nullslice;
            } else {
                fleece::slice_istream number(json);
                auto n = number.readDecimal();
                if (number.size == 0)
                    _value = n;
                else
                    _value = fleece::alloc_slice(json);
            }
        }

        explicit operator bool() const noexcept FLPURE {return isInt() || sliceValue();}
        bool isInt() const noexcept FLPURE            {return std::holds_alternative<uint64_t>(_value);}
        uint64_t intValue() const FLPURE              {return *std::get_if<uint64_t>(&_value);}
        const fleece::alloc_slice& sliceValue() const FLPURE  {
            return *std::get_if<fleece::alloc_slice>(&_value);
        }

        fleece::alloc_slice toJSON() const {
            if (isInt()) {
                char buf[30];
                sprintf(buf, "%" PRIu64, intValue());
                return fleece::alloc_slice(buf);
            } else {
                return sliceValue();
            }
        }

        std::string toJSONString() const {
            if (isInt())
                return format("%" PRIu64, intValue());
            else
                return string(sliceValue());
        }

        bool operator== (const RemoteSequence &other) const noexcept FLPURE {
            return _value == other._value;
        }

        bool operator!= (const RemoteSequence &other) const noexcept FLPURE {
            return _value != other._value;
        }

        bool operator< (const RemoteSequence &other) const noexcept FLPURE {
            if (isInt())
                return !other.isInt() || intValue() < other.intValue();
            else
                return !other.isInt() && sliceValue() < other.sliceValue();
        }

    private:
        std::variant<uint64_t, fleece::alloc_slice> _value;
    };

}
