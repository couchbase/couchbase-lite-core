//
//  Collatable.mm
//  CBForest
//
//  Created by Jens Alfke on 6/11/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#import <Foundation/Foundation.h>
#import "Collatable.hh"
#import "error.hh"


namespace cbforest {

    CollatableBuilder::CollatableBuilder(__unsafe_unretained id obj)
    :_buf(slice::newBytes(kDefaultSize), kDefaultSize),
     _available(_buf)
    {
        if (obj)
            *this << obj;
    }


    CollatableBuilder& CollatableBuilder::operator<< (__unsafe_unretained id obj) {
        if ([obj isKindOfClass: [NSString class]]) {
            *this << nsstring_slice(obj);
        } else if ([obj isKindOfClass: [NSNumber class]]) {
            switch ([obj objCType][0]) {
                case 'c':
                    addBool([obj boolValue]);
                    break;
                case 'f':
                case 'd':
                    *this << [obj doubleValue];
                    break;
                case 'Q':
                    *this << [obj unsignedLongLongValue];
                    break;
                default:
                    *this << [obj longLongValue];
                    break;
            }
        } else if ([obj isKindOfClass: [NSArray class]]) {
            beginArray();
            for (id item in obj) {
                *this << item;
            }
            endArray();
        } else if ([obj isKindOfClass: [NSDictionary class]]) {
            beginMap();
            for (NSString* key in obj) {
                *this << nsstring_slice(key);
                *this << [obj objectForKey: key];
            }
            endMap();
        } else if ([obj isKindOfClass: [NSNull class]]) {
            addNull();
        } else {
            NSCAssert(NO, @"Objects of class %@ are not JSON-compatible", [obj class]);
        }
        return *this;
    }


    static NSString* decodeNSString(slice buf, slice &data) {
        const uint8_t* toChar = CollatableReader::getInverseCharPriorityMap();
        for (int i=0; i<buf.size; i++)
            (uint8_t&)buf[i] = toChar[data[i]];
        data.moveStart(buf.size+1);
        return (NSString*)buf;
    }


    NSString* CollatableReader::readNSString() {
        if (peekTag() == kGeohash)
            expectTag(kGeohash);
        else
            expectTag(kString);
        const void* end = _data.findByte(0);
        if (!end)
            throw error(error::CorruptIndexData); // malformed string
        size_t nBytes = _data.offsetOf(end);

        if (nBytes > 1024) {
            alloc_slice buf(nBytes);
            return decodeNSString(buf, _data);
        } else {
            uint8_t buf[nBytes];
            return decodeNSString(slice(buf, nBytes), _data);
        }
    }


    id CollatableReader::readNSObject() {
        if (_data.size == 0)
            return nil;
        switch (peekTag()) {
            case kNull:
                skipTag();
                return [NSNull null];
            case kFalse:
                skipTag();
                return @NO;
            case kTrue:
                skipTag();
                return @YES;
            case kNegative:
            case kPositive:
                return @(readDouble());
            case kString:
            case kGeohash:
                return readNSString();
            case kArray: {
                beginArray();
                NSMutableArray* result = [NSMutableArray array];
                while (peekTag() != 0)
                    [result addObject: readNSObject()];
                endArray();
                return result;
            }
            case kMap: {
                beginMap();
                NSMutableDictionary* result = [NSMutableDictionary dictionary];
                while (peekTag() != 0) {
                    NSString* key = (NSString*)readString();
                    result[key] = readNSObject();
                }
                endMap();
                return result;
            }
            case kSpecial:
                throw error(error::CorruptIndexData); // can't convert Special tag to NSObject
            default:
                throw error(error::CorruptIndexData); // invalid tag in Collatable data
        }
    }


}