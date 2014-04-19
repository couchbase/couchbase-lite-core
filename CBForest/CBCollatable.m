//
//  CBCollatable.m
//  CBForest
//
//  Created by Jens Alfke on 4/9/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBCollatable.h"


/* Here's the data format:
    Each object begins with a 1-byte tag that identifies its type:
    null:   1
    false:  2
    true:   3
    number: 4
    string: 5
    array:  6
    dict:   7
 
    Null, false and true don't need any following data. They're just single bytes.
 
    Numbers are encoded as follows. (Currently only integers are supported.)
    First a length/sign byte:
        for a positive number this is the number of bytes in the numeric value, ORed with 0x80.
        for a negative number this is 127 minus the number of bytes in the numeric value.
    Then the number itself, using a variable number of bytes, in big-endian byte order.
      I know, this sounds weird and complex, but it ensures proper ordering while saving space.
    (A naive encoding would just store an 8-byte big-endian value, but this encoding cuts it
    down to 2 bytes for small integers.)
      To support floating-point I'll probably need to write the exponent first, then write the
    mantissa in a format mostly like the above.
 
    Strings are converted to UTF-8 and then _partially_ remapped to Unicode Collation Algorithm
    order: each ASCII character/byte is replaced with its priority in that ordering. Control
    characters map to 0, whitespace to 1, "`" to 2, etc. (See kInverseMap below for details.)
      Note that each uppercase letter maps just after its matching lowercase letter. That makes
    the comparison mostly case-insensitive, except that uppercase letters break ties.
      It wouldn't be difficult to extend this implementation to support Unicode ordering of other
    Roman-alphabet characters and some common non-ASCII punctuation marks. But the full UCA looks
    too complex to support with a dumb lexicographic sort, unless maybe each character is
    represented by a complex multibyte sequence, which would make strings take up a lot of space.

    Arrays are simple: just encode each object in the array, and end with a zero byte.
 
    Dictionaries are almost as simple: sort the key/value pairs by comparing the key strings in
    Unicode order, encode the key and value of each pair, and then end with a zero byte.
 */


static NSComparisonResult compareCanonStrings( id s1, id s2, void *context);
static BOOL withMutableUTF8(NSString* str, void (^block)(uint8_t*, size_t));
static uint8_t* getCharPriorityMap(void);


NSData* CBCreateCollatable(id object) {
    NSMutableData* output = [NSMutableData dataWithCapacity: 16];
    CBAddCollatable(object, output);
    return output;
}


static void encodeNumber(NSNumber* number, NSMutableData* output) {
    const char* encoding = number.objCType;
    if (encoding[0] == 'c') {
        // Boolean:
        [output appendBytes: ([number boolValue] ? "\3" : "\2") length: 1];
    } else {
        // Integer: Start by encoding the tag, and getting the number:
        [output appendBytes: "\4" length: 1];
        int64_t n = number.longLongValue;

        // Then figure out how many bytes of the big-endian representation we need to encode:
        union {
            int64_t asInt;
            uint8_t bytes[8];
        } val;
        val.asInt = NSSwapHostLongLongToBig(n);
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
        [output appendBytes: &lenByte length: 1];
        [output appendBytes: &val.bytes[i] length: nBytes];
    }
}

static void encodeString(NSString* str, NSMutableData* output) {
    [output appendBytes: "\5" length: 1];
    withMutableUTF8(str, ^(uint8_t *utf8, size_t length) {
        const uint8_t* priority = getCharPriorityMap();
        for (int i=0; i<length; i++)
            utf8[i] = priority[utf8[i]];
        [output appendBytes: utf8 length: length];
    });
    [output appendBytes: "\0" length: 1];
    //FIX: This doesn't match the Unicode Collation Algorithm, for non-ASCII characters.
    // In fact, I think it's impossible to be fully conformant using a 'dumb' lexicographic compare.
    // http://www.unicode.org/reports/tr10/
    // http://www.unicode.org/Public/UCA/latest/allkeys.txt
}

static void encodeArray(NSArray* array, NSMutableData* output) {
    CBCollatableBeginArray(output);
    for (id object in array) {
        CBAddCollatable(object, output);
    }
    CBCollatableEndArray(output);
}

static void encodeDictionary(NSDictionary* dict, NSMutableData* output) {
    [output appendBytes: "\7" length: 1];
    NSArray* keys = [[dict allKeys] sortedArrayUsingFunction: &compareCanonStrings context: NULL];
    for (NSString* key in keys) {
        encodeString(key, output);
        CBAddCollatable(dict[key], output);
    }
    [output appendBytes: "\0" length: 1];
}

void CBAddCollatable(id object, NSMutableData* output) {
    if ([object isKindOfClass: [NSString class]]) {
        encodeString(object, output);
    } else if ([object isKindOfClass: [NSNumber class]]) {
        encodeNumber(object, output);
    } else if ([object isKindOfClass: [NSNull class]]) {
        [output appendBytes: "\1" length: 1];
    } else if ([object isKindOfClass: [NSDictionary class]]) {
        encodeDictionary(object, output);
    } else if ([object isKindOfClass: [NSArray class]]) {
        encodeArray(object, output);
    } else {
        NSCAssert(NO, @"CBAddCollatable can't encode instances of %@", [object class]);
    }
}

void CBCollatableBeginArray(NSMutableData* output)   {[output appendBytes: "\6" length: 1];}
void CBCollatableEndArray(NSMutableData* output)     {[output appendBytes: "\0" length: 1];}


#pragma mark - STRING UTILITIES:


// String comparison callback used by encodeDictionary.
static NSComparisonResult compareCanonStrings( id s1, id s2, void *context) {
    return [s1 compare: s2 options: NSLiteralSearch];
}


// Returns a 256-byte table that maps each ASCII character to its relative priority in Unicode
// ordering. Bytes 0x80--0xFF (i.e. UTF-8 encoded sequences) map to themselves.
static uint8_t* getCharPriorityMap(void) {
    static uint8_t kCharPriority[256];
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        // Control characters have zero priority:
        static const char* const kInverseMap = "\t\n\r `^_-,;:!?.'\"()[]{}@*/\\&#%+<=>|~$0123456789aAbBcCdDeEfFgGhHiIjJkKlLmMnNoOpPqQrRsStTuUvVwWxXyYzZ";
        uint8_t priority = 1;
        for (int i=0; i<strlen(kInverseMap); i++)
            kCharPriority[kInverseMap[i]] = priority++;
        for (int i=128; i<256; i++)
            kCharPriority[i] = (uint8_t)i;
    });
    return kCharPriority;
}


// Calls the given block, passing it a UTF-8 encoded version of the given string.
// The UTF-8 data can be safely modified by the block but isn't valid after the block exits.
static BOOL withMutableUTF8(NSString* str, void (^block)(uint8_t*, size_t)) {
    NSUInteger byteCount;
    if (str.length < 256) {
        // First try to copy the UTF-8 into a smallish stack-based buffer:
        uint8_t stackBuf[256];
        NSRange remaining;
        BOOL ok = [str getBytes: stackBuf maxLength: sizeof(stackBuf) usedLength: &byteCount
                       encoding: NSUTF8StringEncoding options: 0
                          range: NSMakeRange(0, str.length) remainingRange: &remaining];
        if (ok && remaining.length == 0) {
            block(stackBuf, byteCount);
            return YES;
        }
    }

    // Otherwise malloc a buffer to copy the UTF-8 into:
    NSUInteger maxByteCount = [str maximumLengthOfBytesUsingEncoding: NSUTF8StringEncoding];
    uint8_t* buf = malloc(maxByteCount);
    if (!buf)
        return NO;
    BOOL ok = [str getBytes: buf maxLength: maxByteCount usedLength: &byteCount
                   encoding: NSUTF8StringEncoding options: 0
                      range: NSMakeRange(0, str.length) remainingRange: NULL];
    if (ok)
        block(buf, byteCount);
        free(buf);
        return ok;
}
