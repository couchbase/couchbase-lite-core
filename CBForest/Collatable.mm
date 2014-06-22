//
//  Collatable.mm
//  CBForest
//
//  Created by Jens Alfke on 6/11/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <Foundation/Foundation.h>
#import "Collatable.hh"


namespace forestdb {

    Collatable& Collatable::operator<< (id obj) {
        if ([obj isKindOfClass: [NSString class]]) {
            *this << [obj UTF8String];
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
        } else if ([obj isKindOfClass: [NSDictionary class]]) {
            beginMap();
            for (NSString* key in obj) {
                *this << [key UTF8String];
                *this << [obj objectForKey: key];
            }
            endMap();
        } else if ([obj isKindOfClass: [NSArray class]]) {
            beginArray();
            for (NSString* item in obj) {
                *this << item;
            }
            endArray();
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
        expectTag(kString);
        const void* end = _data.findByte(0);
        if (!end)
            throw "malformed string";
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
            case kNumber:
                return @(readInt());
            case kDouble:
                return @(readDouble());
            case kString:
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
            default:
                return nil;
        }
    }


}