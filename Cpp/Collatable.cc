//
//  Collatable.cc
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "Collatable.h"
#include <CoreFoundation/CFByteOrder.h>

namespace forestdb {

    static uint8_t* getCharPriorityMap();
    static uint8_t* getInverseCharPriorityMap();


    Collatable& Collatable::addBool (bool b) {
        addTag(b ? 3 : 2);
        return *this;
    }
    
    Collatable& Collatable::operator<< (int64_t n) {
        // Figure out how many bytes of the big-endian representation we need to encode:
        union {
            int64_t asInt;
            uint8_t bytes[8];
        } val;
        val.asInt = CFSwapInt64HostToBig(n);
        uint8_t ignore = n < 0 ? 0xFF : 0x00;
        int i;
        for (i=0; i<8; i++)
            if (val.bytes[i] != ignore)
                break;
        if (n<0)
            i--;
        uint8_t nBytes = (uint8_t)(8-i);

        // Encode the length/flag byte and then the number itself:
        uint8_t lenByte =  n>=0 ? (0x80 | nBytes) : (127 - nBytes);
        addTag(4);
        add(slice(&lenByte, 1));
        add(slice(&val.bytes[i], nBytes));
        return *this;
    }


    Collatable& Collatable::operator<< (std::string str) {
        const uint8_t* priority = getCharPriorityMap();
        addTag(5);
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
        addTag(5);
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



#pragma mark - READER:


    CollatableReader::Tag CollatableReader::nextTag() const {
        return _data.size ? (Tag)_data[0] : kEndSequence;
    }


    void CollatableReader::expectTag(uint8_t tag) {
        slice tagSlice = _data.read(1);
        if (tagSlice.size == 0)
            throw "unexpected end of collatable data";
        else if (tagSlice[0] != tag)
            throw "unexpected tag";
    }

    int64_t CollatableReader::readInt() {
        expectTag(4);
        unsigned nBytes;
        uint8_t lenByte;
        union {
            int64_t asBigEndian;
            uint8_t asBytes[8];
        } numBuf;
        slice lenByteSlice = _data.read(1);
        if (lenByteSlice.size == 0)
            throw "malformed number";
        lenByte = lenByteSlice[0];
        if (lenByte & 0x80) {
            nBytes = lenByte & 0x7F;
            numBuf.asBigEndian = 0;
        } else {
            nBytes = 127 - lenByte;
            numBuf.asBigEndian = -1;
        }
        if (nBytes > 8)
            throw "malformed number";
        if (!_data.readInto(slice(&numBuf.asBytes[8-nBytes], nBytes)))
            throw "malformed number";
        return CFSwapInt64BigToHost(numBuf.asBigEndian);
    }

    alloc_slice CollatableReader::readString() {
        expectTag(5);
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
            case 4: { // Number:
                uint8_t lenByte = _data.read(1)[0];
                size_t nBytes;
                if (lenByte & 0x80) {
                    nBytes = lenByte & 0x7F;
                } else {
                    nBytes = 127 - lenByte;
                }
                _data.moveStart(nBytes);
                break;
            }
            case 5: { // String:
                const void* end = _data.findByte(0);
                if (!end)
                    throw "malformed string";
                _data.moveStart(_data.offsetOf(end)+1);
                break;
            }
            case 6: { // Array:
                while (_data[0] != 0)
                    read();
                _data.moveStart(1);
                break;
            }
            case 7: { // Dict:
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
        expectTag(6);
    }

    void CollatableReader::endArray() {
        expectTag(0);
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

    static uint8_t* getInverseCharPriorityMap() {
        static uint8_t kMap[256];
        static bool initialized;
        if (!initialized) {
            // Control characters have zero priority:
            uint8_t* priorityMap = getCharPriorityMap();
            for (int i=0; i<256; i++)
                kMap[priorityMap[i]] = (uint8_t)i;
        }
        return kMap;
    }

}