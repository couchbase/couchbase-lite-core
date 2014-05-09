//
//  CBTextTokenizer.h
//  CBForest
//
//  Created by Jens Alfke on 5/2/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <Foundation/Foundation.h>


/** Breaks Unicode text into words and "stems" them (removes tense-specific parts) for indexing.
    This class is thread-safe. */
@interface CBTextTokenizer : NSObject <NSCopying>

/** Initializes based on the current locale's language. */
- (instancetype) init;

/** Initializes based on a specific language.
    @param language  Language to interpret text as, or nil for language-neutral.
    @param removeDiacritics  YES if diacritical marks should be stripped from letters.
    @return Tokenizer, or nil if the language is unrecognized. */
- (instancetype) initWithLanguage: (NSString*)language
                 removeDiacritics: (BOOL)removeDiacritics;

/** A set of words (as NSData objects) that should be ignored and not returned by the tokenizer. */
@property (copy) NSSet* stopWords;

/** A string containing extra characters that should be considered parts of words. */
@property (copy) NSString* tokenCharacters;

/** Tokenizes a string, calling the onToken block once for each non-stopword.
    The first parameter of the block is the token, which will have been stemmed if the language is
    non-nil and there is a stemmer for that language.
    The second parameter is the byte range in the UTF-8-converted string at which the original
    word appears. */
- (BOOL) tokenize: (NSString*)string
           unique: (BOOL)unique
          onToken: (void (^)(NSString* token, NSRange byteRange))onToken;

- (BOOL) tokenize: (NSString*)string
           unique: (BOOL)unique
      onTokenData: (void (^)(NSData* tokenData, NSRange byteRange))onToken;

/** Tokenizes a string, returning a set of unique token strings. */
- (NSSet*) tokenize: (NSString*)string;

/** Frees any cached data, like low-level tokenizer refs. */
- (void) clearCache;

@end
