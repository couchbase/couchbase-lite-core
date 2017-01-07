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
                                     where: (nullable NSArray*)where
                                   orderBy: (nullable NSArray*)sortDescriptors
                                     error: (NSError**)error;

- (instancetype) initWithDatabase: (LCDatabase*)db
                   wherePredicate: (NSPredicate*)where
                          orderBy: (nullable NSArray*)sortDescriptors
                            error: (NSError**)outError;

@property (nonatomic) LCDatabase* database;
@property (nonatomic) NSUInteger skip;
@property (nonatomic) NSUInteger limit;
@property (copy, nonatomic, nullable) NSDictionary* parameters;

- (nullable NSEnumerator<LCQueryRow*>*) run: (NSError**)error;

@end


@interface LCQueryRow : NSObject

@property (readonly, nonatomic) NSString* documentID;
@property (readonly, nonatomic) uint64_t sequence;

@property (readonly, nonatomic) LCDocument* document;

// Full-text queries only:

/** The text emitted when the view was indexed (the argument to CBLTextKey()) which contains the
    match(es). */
@property (readonly, nullable) NSString* fullTextMatched;

/** The number of query words that were found in the fullText.
    (If a query word appears more than once, only the first instance is counted.) */
@property (readonly, nonatomic) NSUInteger matchCount;

/** The character range in the fullText of a particular match. */
- (NSRange) textRangeOfMatch: (NSUInteger)matchNumber;

/** The index of the search term matched by a particular match. Search terms are the individual
    words in the full-text search expression, skipping duplicates and noise/stop-words. They're
    numbered from zero. */
- (NSUInteger) termIndexOfMatch: (NSUInteger)matchNumber;

@end


NS_ASSUME_NONNULL_END
