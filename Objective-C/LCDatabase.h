//
//  LCDatabase.h
//  LiteCore
//
//  Created by Jens Alfke on 10/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#import <Foundation/Foundation.h>


/** LiteCore database object. */
@interface LCDatabase : NSObject

- (instancetype) initWithPath: (NSString*)directory
                        error: (NSError**)outError;

- (bool) close: (NSError**)outError;

@end
