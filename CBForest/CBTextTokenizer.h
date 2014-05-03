//
//  CBTextTokenizer.h
//  CBForest
//
//  Created by Jens Alfke on 5/2/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <Foundation/Foundation.h>
#import "slice.h"


/** Breaks Unicode text into words and "stems" them (removes tense-specific parts) for indexing. */
@interface CBTextTokenizer : NSObject

/** Initializes based on the current locale's language. */
- (instancetype) init;

/** Initializes based on a specific language.
    @param language  Language to interpret text as, or nil for language-neutral.
    @param removeDiacritics  YES if diacritical marks should be stripped from letters.
    @return Tokenizer, or nil if the language is unrecognized. */
- (instancetype) initWithLanguage: (NSString*)language
                 removeDiacritics: (BOOL)removeDiacritics;

/** A set of words that should be ignored and not returned by the tokenizer. */
@property (copy) NSSet* stopWords;

/** Tokenizes a string, calling the onToken block once for each word. */
- (BOOL) tokenize: (NSString*)string
          onToken: (void (^)(NSString* word, NSRange byteRange))onToken;

@end
