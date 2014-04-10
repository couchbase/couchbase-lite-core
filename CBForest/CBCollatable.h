//
//  CBCollatable.h
//  CBForest
//
//  Created by Jens Alfke on 4/9/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <Foundation/Foundation.h>

/** Encodes a JSON-compatible object in a binary form that can be sorted using normal lexicographic
    sort (like memcmp) and will still end up collated in the correct order for view indexes. */
NSData* CBCreateCollatable(id jsonObject);

void CBCollatableBeginArray(NSMutableData* output);
void CBCollatableEndArray(NSMutableData* output);

/** Like CBCreateCollatable but _appends_ the encoded version of the object to an existing mutable
    data object. */
void CBAddCollatable(id jsonObject, NSMutableData* output);
