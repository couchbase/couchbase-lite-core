//
// DeDuplicateEncoder.hh
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
#include "Error.hh"
#include "fleece/Fleece.hh"
#include "fleece/FLExpert.h"
#include <unordered_map>

namespace litecore {

    /** Wrapper around a Fleece Encoder that detects repeated Array/Dict/data values and writes them only
        once. Subsequent appearances of a value are encoded as pointers inside the Fleece data, taking
        up only 4 or 6 bytes. (This turns the encoded data structure into a DAG, but that makes no
        difference to later readers since it's immutable anyway.)

        Note: Arrays and Dicts are compared for _pointer_ equality, Data values byte-by-byte.
             The regular Fleece encoder already de-duplicates strings byte-by-byte. */
    class DeDuplicateEncoder {
    public:
        explicit DeDuplicateEncoder(FLEncoder enc)
        :_enc(enc) { }

        /// Writes a Value to the encoder, substituting a pointer if it's already been written.
        /// @param v  The Fleece value to write.
        /// @param depth  How many levels of nesting to check for duplicates.
        ///              0 means just this Value itself, 1 includes its children, etc.
        void writeValue(fleece::Value v, int depth) {
            DebugAssert(depth >= 0);
            if (auto type = v.type(); type < kFLData) {
                _enc.writeValue(v);
            } else if (auto e = _written.find(v); e != _written.end() && e->second != 0) {
                FLEncoder_WriteValueAgain(_enc, e->second);
            } else switch (type) {
                case kFLData:   writeData(v.asData()); break;
                case kFLArray:  writeArray(v.asArray(), depth); break;
                case kFLDict:   writeDict(v.asDict(), depth); break;
                default:        DebugAssert(false, "Illegal Value type");
            }
        }

    private:
        void writeData(slice data) {
            if (auto e = _writtenData.find(data); e != _writtenData.end() && e->second != 0) {
                FLEncoder_WriteValueAgain(_enc, e->second);
            } else {
                _enc.writeData(data);
                _writtenData[data] = FLEncoder_LastValueWritten(_enc);
            }
        }

        void writeArray(fleece::Array array, int depth) {
            _enc.beginArray(array.count());
            for (fleece::Array::iterator i(array); i; ++i)
            _writeChild(i.value(), depth);
            _enc.endArray();
            _written[array] = FLEncoder_LastValueWritten(_enc);
        }

        void writeDict(fleece::Dict dict, int depth) {
            _enc.beginDict(dict.count());
            for (fleece::Dict::iterator i(dict); i; ++i) {
                _enc.writeKey(i.key());
                _writeChild(i.value(), depth);
            }
            _enc.endDict();
            _written[dict] = FLEncoder_LastValueWritten(_enc);
        }

        void _writeChild(fleece::Value v, int depth) {
            if (depth > 0)
                writeValue(v, depth - 1);
            else
                _enc.writeValue(v);
        }

        fleece::SharedEncoder               _enc;
        std::unordered_map<FLValue,ssize_t> _written;
        std::unordered_map<slice,ssize_t>   _writtenData;
    };

}
