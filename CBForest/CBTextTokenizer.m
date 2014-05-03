//
//  CBTextTokenizer.m
//  CBForest
//
//  Created by Jens Alfke on 5/2/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import "CBTextTokenizer.h"
#import "CBForestPrivate.h"
#import "fts3_unicodesn.h"
#import <sqlite3.h>

#import "english_stopwords.h"


SQLITE_API void *sqlite3_malloc(int size)               {return malloc(size);}
SQLITE_API void *sqlite3_realloc(void* ptr, int size)   {return realloc(ptr, size);}
SQLITE_API void sqlite3_free(void* ptr)                 {free(ptr);}


@implementation CBTextTokenizer
{
    sqlite3_tokenizer* _tokenizer;
}

@synthesize stopWords=_stopWords;


static const sqlite3_tokenizer_module* sModule;
static NSDictionary* sLanguageToStemmer;
static NSDictionary* sLanguageToStopWords;

+ (void) initialize {
    if (!sModule) {
        sqlite3Fts3UnicodeSnTokenizer(&sModule);
        NSAssert(sModule, @"Failed to initialize sqlite3FtsUnicodeSnTokenizer");
        sLanguageToStemmer = @{@"en": @"english"};
        sLanguageToStopWords = @{@"en": [self stopWordsFromString: kEnglishStopWords]};
    }
}


+ (NSSet*) stopWordsFromString: (const char*)cString {
    NSMutableSet* stopWords = [NSMutableSet set];
    const char* space;
    do {
        space = strchr(cString, ' ');
        size_t length = space ? (space-cString) : strlen(cString);
        NSString* word = [[NSString alloc] initWithBytes: cString length: length
                                                encoding: NSUTF8StringEncoding];
        [stopWords addObject: word];
        cString = space+1;
    } while (space);
    return [stopWords copy];
}


- (instancetype) init {
    NSString* language = [[NSLocale currentLocale] objectForKey: NSLocaleLanguageCode];
    return [self initWithLanguage: language
                 removeDiacritics: [language isEqualToString: @"en"]];
}


- (instancetype) initWithLanguage: (NSString*)language
                 removeDiacritics: (BOOL)removeDiacritics
{
    self = [super init];
    if (self) {
        NSString* stemmer = nil;
        if (language) {
            stemmer = sLanguageToStemmer[language];
            _stopWords = sLanguageToStopWords[language];
        }
        const char* argv[10] = {};
        int argc = 0;
        if (!removeDiacritics)
            argv[argc++] = "remove_diacritics=0";
        if (stemmer)
            argv[argc++] = [[NSString stringWithFormat: @"stemmer=%@", stemmer] UTF8String];
        int err = sModule->xCreate(argc, argv, &_tokenizer);
        if (err)
            return nil;
    }
    return self;
}


- (void)dealloc
{
    if (_tokenizer)
        sModule->xDestroy(_tokenizer);
}


- (BOOL) tokenize: (NSString*)string onToken: (void (^)(NSString*,NSRange))onToken {
    __block int err = SQLITE_OK;
    WithMutableUTF8(string, ^(uint8_t *bytes, size_t byteCount) {
        sqlite3_tokenizer_cursor* cursor;
        err = sModule->xOpen(_tokenizer, (const char*)bytes, (int)byteCount, &cursor);
        if (err)
            return;
        cursor->pTokenizer = _tokenizer; // module expects sqlite3 to have initialized this

        do {
            const char *tokenBytes;
            int tokenLength;
            int startOffset, endOffset, pos;
            err = sModule->xNext(cursor, &tokenBytes, &tokenLength, &startOffset, &endOffset, &pos);
            if (err == SQLITE_OK) {
                NSString* token = [[NSString alloc] initWithBytes: tokenBytes length: tokenLength
                                                         encoding: NSUTF8StringEncoding];
                if (![_stopWords containsObject: token])
                    onToken(token, NSMakeRange(startOffset, endOffset-startOffset));
            }
        } while (err == SQLITE_OK);
        if (err == SQLITE_DONE)
            err = SQLITE_OK;

        sModule->xClose(cursor);
    });
    return (err == 0);
}


@end
