//
//  CBCollatable.h
//  CBForest
//
//  Created by Jens Alfke on 4/9/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <Foundation/Foundation.h>
#import "sized_buf.h"


/** Encodes a JSON-compatible object in a binary form that can be sorted using normal lexicographic
    sort (like memcmp) and will still end up collated in the correct order for view indexes. */
NSData* CBCreateCollatable(id jsonObject);

void CBCollatableBeginArray(NSMutableData* output);
void CBCollatableEndArray(NSMutableData* output);

/** Like CBCreateCollatable but _appends_ the encoded version of the object to an existing mutable
    data object. */
void CBAddCollatable(id jsonObject, NSMutableData* output);


typedef enum {
    kEndSequenceType = 0,   // Returned to indicate the end of an array/dict
    kNullType,
    kFalseType,
    kTrueType,
    kNumberType,
    kStringType,
    kArrayType,
    kDictionaryType,
    kErrorType = 255        // Something went wrong...
} CBCollatableType;


/** Reads the next item from collatable data. Scalar values are stored in *output.
    If the type returned is kArrayType or kDictionaryType, no value is placed in *output;
    the function should be called again to get the contents until type kEndSequenceType is
    returned. */
CBCollatableType CBCollatableReadNext(sized_buf *input, BOOL recurse, id *output);

BOOL CBCollatableReadNextNumber(sized_buf *input, int64_t *output);

/** Reads an entire object stored in collatable form. Returns nil on error. */
id CBCollatableRead(sized_buf input);
