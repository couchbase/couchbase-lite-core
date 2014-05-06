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
    NSString* _stemmer;
    BOOL _removeDiacritics;
    NSMutableArray* _tokenizers;
}

@synthesize stopWords=_stopWords, tokenCharacters=_tokenCharacters;


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
        _removeDiacritics = removeDiacritics;
        if (language) {
            _stemmer = sLanguageToStemmer[language];
            _stopWords = sLanguageToStopWords[language];
        }
        _tokenizers = [[NSMutableArray alloc] init];
    }
    return self;
}


- (instancetype) copyWithZone: (NSZone*)zone {
    CBTextTokenizer* tok = [[[self class] alloc] initWithLanguage: nil
                                                 removeDiacritics: _removeDiacritics];
    tok->_stemmer = _stemmer;
    tok.stopWords = _stopWords;
    tok.tokenCharacters = _tokenCharacters;
    return tok;
}


- (void) dealloc {
    for (NSValue* v in _tokenizers)
        [self freeTokenizer: v.pointerValue];
}


- (sqlite3_tokenizer*) createTokenizer {
    const char* argv[10];
    int argc = 0;
    if (!_removeDiacritics)
        argv[argc++] = "remove_diacritics=0";
    if (_stemmer)
        argv[argc++] = [[NSString stringWithFormat: @"stemmer=%@", _stemmer] UTF8String];
    if (_tokenCharacters)
        argv[argc++] = [[NSString stringWithFormat: @"tokenchars=%@", _tokenCharacters] UTF8String];
    sqlite3_tokenizer* tokenizer;
    int err = sModule->xCreate(argc, argv, &tokenizer);
    return err ? NULL : tokenizer;
}

- (void) freeTokenizer: (sqlite3_tokenizer*)tokenizer {
    if (tokenizer)
        sModule->xDestroy(tokenizer);
}


- (sqlite3_tokenizer*) getTokenizer {
    @synchronized(self) {
        if (_tokenizers.count > 0) {
            sqlite3_tokenizer* tokenizer = [_tokenizers[0] pointerValue];
            [_tokenizers removeObjectAtIndex: 0];
            return tokenizer;
        } else {
            return [self createTokenizer];
        }
    }
}

- (void) returnTokenizer: (sqlite3_tokenizer*)tokenizer {
    @synchronized(self) {
        [_tokenizers addObject: [NSValue valueWithPointer: tokenizer]];
    }
}

- (void) clearCache {
    @synchronized(self) {
        [_tokenizers removeAllObjects];
    }
}


- (BOOL) tokenize: (NSString*)string onToken: (void (^)(NSString*,NSRange))onToken {
    sqlite3_tokenizer* tokenizer = [self getTokenizer];
    __block int err = SQLITE_OK;
    WithMutableUTF8(string, ^(uint8_t *bytes, size_t byteCount) {
        sqlite3_tokenizer_cursor* cursor;
        err = sModule->xOpen(tokenizer, (const char*)bytes, (int)byteCount, &cursor);
        if (err)
            return;
        cursor->pTokenizer = tokenizer; // module expects sqlite3 to have initialized this

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
    [self returnTokenizer: tokenizer];
    return (err == SQLITE_OK);
}


- (NSSet*) tokenize: (NSString*)string {
    NSMutableSet* tokens = [NSMutableSet set];
    [self tokenize: string onToken: ^(NSString* token, NSRange r) {
        [tokens addObject: token];
    }];
    return tokens;
}


@end
