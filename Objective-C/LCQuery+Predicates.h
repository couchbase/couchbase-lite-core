//
//  LCQuery+Predicates.h
//  LiteCore
//
//  Created by Jens Alfke on 1/10/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#import "LCQuery.h"

@interface LCQuery (Predicates)

/** Converts an NSPredicate into a JSON-compatible object tree of a LiteCore query. */
+ (id) encodePredicate: (NSPredicate*)pred
                 error: (NSError**)outError;

@end
