//
//  LCQuery.h
//  LiteCore
//
//  Created by Jens Alfke on 11/30/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#import <Foundation/Foundation.h>
@class LCDatabase, LCQueryRow, LCDocument;

NS_ASSUME_NONNULL_BEGIN


/********
 NOTE: THIS IS A PROVISIONAL, PLACEHOLDER API, NOT THE OFFICIAL COUCHBASE LITE 2.0 API.
 It's for prototyping, experimentation, and performance testing. It will change without notice.
 Once the 2.0 API is designed, we will begin implementing that and remove these classes.
 ********/


@interface LCQuery : NSObject

- (nullable instancetype) initWithDatabase: (LCDatabase*)db
                                     where: (NSDictionary*)where
                                   orderBy: (nullable NSArray*)sortDescriptors
                                     error: (NSError**)error;

@property (nonatomic) NSUInteger skip;
@property (nonatomic) NSUInteger limit;
@property (copy, nonatomic, nullable) NSDictionary* parameters;

- (nullable NSEnumerator<LCQueryRow*>*) run: (NSError**)error;

@end


@interface LCQueryRow : NSObject

@property (readonly, nonatomic) NSString* documentID;
@property (readonly, nonatomic) uint64_t sequence;

@property (readonly, nonatomic) LCDocument* document;

@end


NS_ASSUME_NONNULL_END
