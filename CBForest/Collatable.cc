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
#include "Geohash.hh"
#include "Error.hh"
#include <sstream>
#include <iomanip> // std::setprecision
#include <algorithm> // std::max for MSVC

namespace cbforest {

    static uint8_t kCharPriority[256];
    static uint8_t kCharInversePriority[256];

    static void initCharPriorityMap();
    static bool sCharPriorityMapInitialized;


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



    CollatableBuilder::CollatableBuilder()
    :_buf(slice::newBytes(kDefaultSize), kDefaultSize),
     _available(_buf)
    { }

    CollatableBuilder::CollatableBuilder(slice s, bool)
    :_buf(s.copy())
    { }

    CollatableBuilder::CollatableBuilder(Collatable c)
    :_buf(c.copy())
    { }

    CollatableBuilder::~CollatableBuilder() {
        ::free((void*)_buf.buf);
    }

    uint8_t* CollatableBuilder::reserve(size_t amt) {
        if (_available.size < amt) {
            CBFAssert(_buf.buf);
            // grow:
            size_t curSize = size();
            size_t newSize = std::max(_buf.size, kMinSize/2);
            do {
                newSize *= 2;
            } while (newSize < curSize + amt);
            void* newBuf = slice::reallocBytes((void*)_buf.buf, newSize);
            _buf = _available = slice(newBuf, newSize);
            _available.moveStart(curSize);
        }
        auto result = (uint8_t*)_available.buf;
        _available.moveStart(amt);
        return result;
    }

    void CollatableBuilder::add(slice s) {
        ::memcpy(reserve(s.size), s.buf, s.size);
    }

    CollatableBuilder& CollatableBuilder::addBool (bool b) {
        addTag(b ? kTrue : kFalse);
        return *this;
    }
    
    CollatableBuilder& CollatableBuilder::operator<< (double n) {
        addTag(n < 0.0 ? kNegative : kPositive);
        swappedDouble swapped = _encdouble(n);
        if (n < 0.0)
            _invertDouble(swapped);
        add(slice(&swapped, sizeof(swapped)));
        return *this;
    }

    void CollatableBuilder::addString(Tag t, slice s) {
        if (!sCharPriorityMapInitialized)
            initCharPriorityMap();
        auto dst = reserve(2 + s.size);
        *dst++ = t;
        for (size_t i = 0; i < s.size; i++) {
            *dst++ = kCharPriority[s[i]];
        }
        *dst = '\0';
    }

    CollatableBuilder& CollatableBuilder::addFullTextKey(slice text, slice languageCode) {
        addString(kFullTextKey, languageCode);
        addString(kString, text);
        return *this;
    }


    CollatableBuilder& CollatableBuilder::addGeoKey(slice geoJSON, geohash::area bbox) {
        addTag(kGeoJSONKey);
        *this << geoJSON << bbox.min().longitude << bbox.min().latitude
                         << bbox.max().longitude << bbox.max().latitude;
        return *this;
    }


    CollatableBuilder& CollatableBuilder::operator<< (const CollatableBuilder& coll) {
        add(coll);
        return *this;
    }

    CollatableBuilder& CollatableBuilder::operator<< (const Collatable& coll) {
        add(coll);
        return *this;
    }

    std::string CollatableBuilder::toJSON() const {
        return CollatableReader(*this).toJSON();
    }

    std::string Collatable::toJSON() const {
        return CollatableReader(*this).toJSON();
    }



#pragma mark - READER:


    CollatableReader::CollatableReader(slice s)
    :_data(s)
    {
        getInverseCharPriorityMap(); // make sure it's initialized
    }


    CollatableReader::Tag CollatableReader::peekTag() const {
        return _data.size ? (Tag)_data[0] : kEndSequence;
    }


    void CollatableReader::expectTag(Tag tag) {
        slice tagSlice = _data.read(1);
        if (tagSlice.size == 0)
            throw error(error::CorruptIndexData); // unexpected end of collatable data
        else if (tagSlice[0] != tag)
            throw error(error::CorruptIndexData); // unexpected tag"
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
            throw error(error::CorruptIndexData); // unexpected tag
        swappedDouble swapped;
        _data.readInto(slice(&swapped, sizeof(swapped)));
        if (tagSlice[0] == kNegative)
            _invertDouble(swapped);
        return _decdouble(swapped);
    }

    geohash::hash CollatableReader::readGeohash() {
        return geohash::hash(readString(kGeohash));
    }

    alloc_slice CollatableReader::readString(Tag tag) {
        expectTag(tag);
        const void* end = _data.findByte(0);
        if (!end)
            throw error(error::CorruptIndexData); // malformed string
        size_t nBytes = _data.offsetOf(end);

        alloc_slice result(nBytes);

        for (int i=0; i<nBytes; i++)
            (uint8_t&)result[i] = kCharInversePriority[_data[i]];
        _data.moveStart(nBytes+1);
        return result;
    }

    std::pair<alloc_slice, alloc_slice> CollatableReader::readFullTextKey() {
        auto langCode = readString(kFullTextKey);
        return {readString(kString), langCode};
    }

    alloc_slice CollatableReader::readGeoKey(geohash::area &outBBox) {
        expectTag(kGeoJSONKey);
        alloc_slice geoJSON = readString();
        outBBox.longitude.min = readDouble();
        outBBox.latitude.min  = readDouble();
        outBBox.longitude.max = readDouble();
        outBBox.latitude.max  = readDouble();
        return geoJSON;
    }
    
    slice CollatableReader::read() {
        const void* start = _data.buf;
        switch(_data.read(1)[0]) {
            case kNull:
            case kFalse:
            case kTrue:
                break;
            case kNegative:
            case kPositive:
                _data.moveStart(sizeof(double));
                break;
            case kString:
            case kGeohash: {
                const void* end = _data.findByte(0);
                if (!end)
                    throw error(error::CorruptIndexData); // malformed string
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
            case kSpecial:
                break;
            default:
                throw error(error::CorruptIndexData); // Unexpected tag in read()
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

    static void writeJSONString(std::ostream& out, slice str) {
        out << "\"";
        auto start = (const uint8_t*)str.buf;
        auto end = (const uint8_t*)str.end();
        for (auto p = start; p < end; p++) {
            uint8_t ch = *p;
            if (ch == '"' || ch == '\\' || ch < 32 || ch == 127) {
                // Write characters from start up to p-1:
                out << std::string((char*)start, p-start);
                start = p + 1;
                switch (ch) {
                    case '"':
                    case '\\':
                        out << "\\";
                        --start; // ch will be written in next pass
                        break;
                    case '\n':
                        out << "\\n";
                        break;
                    case '\t':
                        out << "\\t";
                        break;
                    default: {
                        char buf[7];
                        sprintf(buf, "\\u%04u", (unsigned)ch);
                        out << buf;
                        break;
                    }
                }
            }
        }
        out << std::string((char*)start, end-start);
        out << "\"";
    }

    void CollatableReader::writeJSONTo(std::ostream &out) {
        if (_data.size == 0) {
            return;
        }
        switch(peekTag()) {
            case kNull:
                _skipTag();
                out << "null";
                break;
            case kFalse:
                _skipTag();
                out << "false";
                break;
            case kTrue:
                _skipTag();
                out << "true";
                break;
            case kNegative:
            case kPositive:
                // default precision is around 5, it makes double number rounded.
                // double's precision should be 16.
                out << std::setprecision(16) << readDouble();
                break;
            case kString:
                writeJSONString(out, readString());
                break;
            case kArray: {
                out << '[';
                beginArray();
                bool first = true;
                while (peekTag() != kEndSequence) {
                    if (first)
                        first = false;
                    else
                        out << ",";
                    writeJSONTo(out);
                }
                endArray();
                out << ']';
                break;
            }
            case kMap: {
                out << '{';
                beginMap();
                bool first = true;
                while (peekTag() != kEndSequence) {
                    if (first)
                        first = false;
                    else
                        out << ",";
                    writeJSONTo(out);
                    out << ':';
                    writeJSONTo(out);
                }
                out << '}';
                endMap();
                break;
            }
            case kSpecial:
                out << "<special>";
                break;
            case kGeohash:
                out << "geohash(" << (std::string)readGeohash() << ')';
                break;
            default:
                out << "¿" << (int)peekTag() << "?";
                break;
        }
    }

    std::string CollatableReader::toJSON() {
        std::stringstream out;
        writeJSONTo(out);
        return out.str();
    }


#pragma mark - UTILITIES:


    // Returns a 256-byte table that maps each ASCII character to its relative priority in Unicode
    // ordering. Bytes 0x80--0xFF (i.e. UTF-8 encoded sequences) map to themselves.
    // The table cannot contain any 0 values, because 0 is reserved as an end-of-string marker.
    static void initCharPriorityMap() {
        if (!sCharPriorityMapInitialized) {
            static const char* const kInverseMap = "\t\n\r `^_-,;:!?.'\"()[]{}@*/\\&#%+<=>|~$0123456789aAbBcCdDeEfFgGhHiIjJkKlLmMnNoOpPqQrRsStTuUvVwWxXyYzZ";
            uint8_t priority = 1;
            for (int i=0; i<strlen(kInverseMap); i++)
                kCharPriority[(uint8_t)kInverseMap[i]] = priority++;
            for (int i=0; i<127; i++)
                if (kCharPriority[i] == 0)
                    kCharPriority[i] = priority++;  // fill in ctrl chars
            kCharPriority[127] = kCharPriority[(int)' '];// and DEL (there's no room for a unique #)
            for (int i=128; i<256; i++)
                kCharPriority[i] = (uint8_t)i;
            sCharPriorityMapInitialized = true;
        }
    }

    uint8_t* CollatableReader::getInverseCharPriorityMap() {
        static bool initialized;
        if (!initialized) {
            initCharPriorityMap();
            for (int i=255; i>=0; i--)
                kCharInversePriority[kCharPriority[i]] = (uint8_t)i;
            initialized = true;
        }
        return kCharInversePriority;
    }

}
