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
