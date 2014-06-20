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


    id CollatableReader::readNSObject() {
        switch (nextTag()) {
            case kNull:
                return [NSNull null];
            case kFalse:
                return @NO;
            case kTrue:
                return @YES;
            case kNumber:
                return @(readInt());
            case kDouble:
                return @(readDouble());
            case kString:
                return (NSString*)readString();
            case kArray: {
                beginArray();
                NSMutableArray* result = [NSMutableArray array];
                while (nextTag() != 0)
                    [result addObject: readNSObject()];
                endArray();
                return result;
            }
            case kMap: {
                beginMap();
                NSMutableDictionary* result = [NSMutableDictionary dictionary];
                while (nextTag() != 0) {
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