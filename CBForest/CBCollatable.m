//
//  CBCollatable.m
//  CBForest
//
//  Created by Jens Alfke on 4/9/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBCollatable.h"
#import "slice.h"
#import "CBForestPrivate.h"


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
static uint8_t* getCharPriorityMap(void);
static uint8_t* getInverseCharPriorityMap(void);


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
    WithMutableUTF8(str, ^(uint8_t *utf8, size_t length) {
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

static void encodeStringData(NSData* str, NSMutableData* output) {
    size_t length = str.length;
    const uint8_t* utf8 = str.bytes;
    uint8_t* buffer = (length < 512) ? alloca(length+2) : malloc(length+2);
    
    buffer[0] = '\5';
    const uint8_t* priority = getCharPriorityMap();
    for (int i=0; i<length; i++)
        buffer[i+1] = priority[utf8[i]];
    buffer[length+1] = '\0';

    [output appendBytes: buffer length: length+2];

    if (length >= 512)
        free(buffer);
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
    } else if ([object isKindOfClass: [NSData class]]) {
        // Interpret an NSData as a UTF-8 string. This saves time if the caller already has a
        // string in this form.
        encodeStringData(object, output);
    } else {
        NSCAssert(NO, @"CBAddCollatable can't encode instances of %@", [object class]);
    }
}

void CBCollatableBeginArray(NSMutableData* output)   {[output appendBytes: "\6" length: 1];}
void CBCollatableEndArray(NSMutableData* output)     {[output appendBytes: "\0" length: 1];}


#pragma mark - DECODING:


// Reads 'size' bytes from 'input' to 'output', advancing 'input' past the read bytes.
static void uncheckedDecodeValue(slice* input, size_t size, void* output) {
    memcpy(output, input->buf, size);
    input->buf += size;
    input->size -= size;
}

static BOOL decodeValue(slice* input, size_t size, void* output) {
    if (input->size < size)
        return NO;
    uncheckedDecodeValue(input, size, output);
    return YES;
}

#define DECODE(INPUT,OUTPUT) decodeValue((INPUT), sizeof(*(OUTPUT)), (OUTPUT))


static BOOL decodeNumber(slice* input, int64_t *outNumber) {
    unsigned nBytes;
    uint8_t lenByte;
    union {
        int64_t asBigEndian;
        uint8_t asBytes[8];
    } numBuf;
    if (!DECODE(input, &lenByte))
        return NO;
    if (lenByte & 0x80) {
        nBytes = lenByte & 0x7F;
        numBuf.asBigEndian = 0;
    } else {
        nBytes = 127 - lenByte;
        numBuf.asBigEndian = -1;
    }
    if (nBytes > 8)
        return NO;
    if (!decodeValue(input, nBytes, &numBuf.asBytes[8-nBytes]))
        return NO;
    *outNumber = NSSwapBigLongLongToHost(numBuf.asBigEndian);
    return YES;
}


static BOOL decodeString(slice* input, NSString** outString) {
    // Find the length:
    void* end = memchr(input->buf, '\0', input->size);
    if (!end)
        return NO;
    size_t nBytes = end - input->buf;
    uint8_t tempBuf[256];
    uint8_t* temp;
    if (nBytes <= sizeof(tempBuf))
        temp = tempBuf;
    else {
        temp = malloc(nBytes);
        if (!temp)
            return NO;
    }
    uncheckedDecodeValue(input, nBytes, temp);
    input->buf++; // consume null byte
    input->size--;
    const uint8_t* toChar = getInverseCharPriorityMap();
    for (int i=0; i<nBytes; i++)
        temp[i] = toChar[temp[i]];
    *outString = [[NSString alloc] initWithBytes: temp length: nBytes encoding: NSUTF8StringEncoding];
    if (temp != tempBuf)
        free(temp);
    return (*outString != NULL);
}


BOOL CBCollatableReadNextNumber(slice *input, int64_t *output) {
    uint8_t type;
    return DECODE(input, &type) && type == kNumberType && decodeNumber(input, output);
}


CBCollatableType CBCollatableReadNext(slice *input, BOOL recurse, id *output) {
    uint8_t type;
    if (!DECODE(input, &type))
        return kErrorType;
    switch(type) {
        case kNullType:
            *output = (id)kCFNull;
            break;
        case kFalseType:
            *output = (id)kCFBooleanFalse;
            break;
        case kTrueType:
            *output = (id)kCFBooleanTrue;
            break;
        case kNumberType: {
            int64_t number;
            if (!decodeNumber(input, &number))
                return kErrorType;
            *output = @(number);
            break;
        }
        case kStringType:
            if (!decodeString(input, output))
                return kErrorType;
            break;
        case kEndSequenceType:
            *output = nil;
            break;
        case kArrayType: {
            if (!recurse) {
                *output = nil;
                break;
            }
            NSMutableArray* result = [NSMutableArray array];
            for(;;) {
                id item;
                CBCollatableType subtype = CBCollatableReadNext(input, YES, &item);
                if (subtype == kErrorType)
                    return kErrorType;
                else if (subtype == kEndSequenceType)
                    break;
                [result addObject: item];
            }
            *output = result;
            break;
        }
        case kDictionaryType: {
            if (!recurse) {
                *output = nil;
                break;
            }
            NSMutableDictionary* result = [NSMutableDictionary dictionary];
            for(;;) {
                id key, value;
                CBCollatableType subtype = CBCollatableReadNext(input, NO, &key);
                if (subtype == kEndSequenceType)
                    break;
                else if (subtype != kStringType)
                    return kErrorType;
                subtype = CBCollatableReadNext(input, YES, &value);
                if (subtype == kErrorType || subtype == kEndSequenceType)
                    return kErrorType;
                result[key] = value;
            }
            *output = result;
            break;
        }
        default:
            return kErrorType;
    }
    return type;
}


id CBCollatableRead(slice input) {
    id output;
    if (CBCollatableReadNext(&input, YES, &output) == kErrorType || input.size > 0)
        output = nil;
    return output;
}


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
            kCharPriority[(uint8_t)kInverseMap[i]] = priority++;
        for (int i=128; i<256; i++)
            kCharPriority[i] = (uint8_t)i;
    });
    return kCharPriority;
}


static uint8_t* getInverseCharPriorityMap(void) {
    static uint8_t kMap[256];
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        // Control characters have zero priority:
        uint8_t* priorityMap = getCharPriorityMap();
        for (int i=0; i<256; i++)
            kMap[priorityMap[i]] = (uint8_t)i;
    });
    return kMap;
}
