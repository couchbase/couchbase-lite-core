//
//  Collatable.cc
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Collatable.hh"
#include "forestdb_endian.h"
#include <sstream>

namespace forestdb {

    union swappedDouble {
        double asDouble;
        uint64_t asRaw;
    };

    static inline swappedDouble _encdouble(double d) {
        swappedDouble swapped;
        swapped.asDouble = d;
        swapped.asRaw = _enc64(swapped.asRaw);
        return swapped;
    }

    static inline double _decdouble(swappedDouble swapped) {
        swapped.asRaw = _dec64(swapped.asRaw);
        return swapped.asDouble;
    }

    static inline void _invertDouble(swappedDouble& swapped) {
        swapped.asRaw ^= 0xFFFFFFFFFFFFFFFF;
    }

    static uint8_t* getCharPriorityMap();


    Collatable::Collatable()
    { }

    Collatable::Collatable(slice s, bool)
    :_str((std::string)s)
    { }

    Collatable& Collatable::addBool (bool b) {
        addTag(b ? kTrue : kFalse);
        return *this;
    }
    
    Collatable& Collatable::operator<< (double n) {
        addTag(n < 0.0 ? kNegative : kPositive);
        swappedDouble swapped = _encdouble(n);
        if (n < 0.0)
            _invertDouble(swapped);
        add(slice(&swapped, sizeof(swapped)));
        return *this;
    }

    Collatable& Collatable::operator<< (std::string str) {
        const uint8_t* priority = getCharPriorityMap();
        addTag(kString);
        size_t first = _str.length();
        _str += str;
        for (auto c=_str.begin()+first; c != _str.end(); ++c) {
            *c = priority[(uint8_t)*c];
        }
        _str.push_back(0);
        return *this;
    }

    Collatable& Collatable::operator<< (slice s) {
        const uint8_t* priority = getCharPriorityMap();
        addTag(kString);
        size_t first = _str.length();
        add(s);
        for (auto c=_str.begin()+first; c != _str.end(); ++c) {
            *c = priority[(uint8_t)*c];
        }
        _str.push_back(0);
        return *this;
    }

    Collatable& Collatable::operator<< (const Collatable& coll) {
        _str += coll._str;
        return *this;
    }

    std::string Collatable::dump() {
        return CollatableReader(*this).dump();
    }



#pragma mark - READER:


    CollatableReader::Tag CollatableReader::peekTag() const {
        return _data.size ? (Tag)_data[0] : kEndSequence;
    }


    void CollatableReader::expectTag(Tag tag) {
        slice tagSlice = _data.read(1);
        if (tagSlice.size == 0)
            throw "unexpected end of collatable data";
        else if (tagSlice[0] != tag)
            throw "unexpected tag";
    }

    int64_t CollatableReader::readInt() {
        double d = readDouble();
        int64_t i = (int64_t)d;
        if (i != d)
            throw("non-integer");
        return i;
    }

    double CollatableReader::readDouble() {
        slice tagSlice = _data.read(1);
        if (tagSlice[0] != kNegative && tagSlice[0] != kPositive)
            throw "unexpected tag";
        swappedDouble swapped;
        _data.readInto(slice(&swapped, sizeof(swapped)));
        if (tagSlice[0] == kNegative)
            _invertDouble(swapped);
        return _decdouble(swapped);
    }

    alloc_slice CollatableReader::readString() {
        expectTag(kString);
        const void* end = _data.findByte(0);
        if (!end)
            throw "malformed string";
        size_t nBytes = _data.offsetOf(end);

        alloc_slice result(nBytes);

        const uint8_t* toChar = getInverseCharPriorityMap();
        for (int i=0; i<nBytes; i++)
            (uint8_t&)result[i] = toChar[_data[i]];
        _data.moveStart(nBytes+1);
        return result;
    }

    slice CollatableReader::read() {
        const void* start = _data.buf;
        switch(_data.read(1)[0]) {
            case kNegative:
            case kPositive:
                _data.moveStart(sizeof(double));
                break;
            case kString: {
                const void* end = _data.findByte(0);
                if (!end)
                    throw "malformed string";
                _data.moveStart(_data.offsetOf(end)+1);
                break;
            }
            case kArray: {
                while (_data[0] != 0)
                    read();
                _data.moveStart(1);
                break;
            }
            case kMap: {
                while (_data[0] != 0) {
                    read();
                    read();
                }
                _data.moveStart(1);
                break;
            }
        }
        return slice(start, _data.buf);
    }

    void CollatableReader::beginArray() {
        expectTag(kArray);
    }

    void CollatableReader::endArray() {
        expectTag(kEndSequence);
    }
    
    void CollatableReader::beginMap() {
        expectTag(kMap);
    }

    void CollatableReader::endMap() {
        expectTag(kEndSequence);
    }

    void CollatableReader::dumpTo(std::ostream &out) {
        switch(peekTag()) {
            case kNull:
                skipTag();
                out << "null";
                break;
            case kFalse:
                skipTag();
                out << "false";
                break;
            case kTrue:
                skipTag();
                out << "true";
                break;
            case kNegative:
            case kPositive:
                out << readDouble();
                break;
            case kString:
                out << '"' << (std::string)readString() << '"';
                break;
            case kArray:
                out << '[';
                beginArray();
                while (peekTag() != kEndSequence)
                    dumpTo(out);
                endArray();
                out << ']';
                break;
            case kMap:
                out << '{';
                beginMap();
                while (peekTag() != kEndSequence) {
                    dumpTo(out);
                    out << ':';
                    dumpTo(out);
                }
                out << '}';
                endMap();
                break;
            case kSpecial:
                out << "<special>";
                break;
            default:
                out << "???";
                break;
        }
    }

    std::string CollatableReader::dump() {
        std::stringstream out;
        dumpTo(out);
        return out.str();
    }


#pragma mark - UTILITIES:


    // Returns a 256-byte table that maps each ASCII character to its relative priority in Unicode
    // ordering. Bytes 0x80--0xFF (i.e. UTF-8 encoded sequences) map to themselves.
    static uint8_t* getCharPriorityMap() {
        static uint8_t kCharPriority[256];
        static bool initialized;
        if (!initialized) {
            // Control characters have zero priority:
            static const char* const kInverseMap = "\t\n\r `^_-,;:!?.'\"()[]{}@*/\\&#%+<=>|~$0123456789aAbBcCdDeEfFgGhHiIjJkKlLmMnNoOpPqQrRsStTuUvVwWxXyYzZ";
            uint8_t priority = 1;
            for (int i=0; i<strlen(kInverseMap); i++)
                kCharPriority[(uint8_t)kInverseMap[i]] = priority++;
            for (int i=128; i<256; i++)
                kCharPriority[i] = (uint8_t)i;
            initialized = true;
        }
        return kCharPriority;
    }

    uint8_t* CollatableReader::getInverseCharPriorityMap() {
        static uint8_t kMap[256];
        static bool initialized;
        if (!initialized) {
            // Control characters have zero priority:
            uint8_t* priorityMap = getCharPriorityMap();
            for (int i=0; i<256; i++)
                kMap[priorityMap[i]] = (uint8_t)i;
            initialized = true;
        }
        return kMap;
    }

}