//
//  Tokenizer.cpp
//  CBForest
//
//  Created by Jens Alfke on 10/17/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Tokenizer.hh"
#include "english_stopwords.h"
#include <assert.h>

extern "C" {
#include "fts3_tokenizer.h"
#include "fts3_unicodesn.h"

    void *sqlite3_malloc(int size)              {return malloc(size);}
    void *sqlite3_realloc(void* ptr, int size)  {return realloc(ptr, size);}
    void sqlite3_free(void* ptr)                {return free(ptr);}

}

namespace forestdb {

    static const struct sqlite3_tokenizer_module* sModule;
    static std::unordered_map<std::string, std::string> sLanguageToStemmer;
    static std::unordered_map<std::string, word_set> sLanguageToStopwords;

    // Reads a space-delimited list of words from a C string (as found in english_stopwords.h)
    static word_set readWordList(const char* cString) {
        word_set stopwords;
        const char* space;
        do {
            space = strchr(cString, ' ');
            size_t length = space ? (space-cString) : strlen(cString);
            std::string word(cString, length);
            stopwords[word] = true;
            cString = space+1;
        } while (space);
        return stopwords;
    }

    Tokenizer::Tokenizer(std::string language, bool removeDiacritics)
    :_language(language),
     _tokenizer(NULL)
    {
        if (!sModule) { //FIX: Make this thread-safe
            sqlite3Fts3UnicodeSnTokenizer(&sModule);
            sLanguageToStemmer["en"] = "english";
            sLanguageToStopwords["en"] = readWordList(kEnglishStopWords);
        }
        _stemmer = sLanguageToStemmer[language];
        _removeDiacritics = removeDiacritics;
    }

    Tokenizer::~Tokenizer() {
        if (_tokenizer)
            sModule->xDestroy(_tokenizer);
    }

    sqlite3_tokenizer* Tokenizer::createTokenizer() {
        const char* argv[10];
        int argc = 0;
        if (!_removeDiacritics)
            argv[argc++] = "remove_diacritics=0";
        std::string stemmerArg, tokenCharsArg;
        if (_stemmer.length() > 0) {
            stemmerArg = std::string("stemmer=") + _stemmer;
            argv[argc++] = stemmerArg.c_str();
        }
        if (_tokenChars.length() > 0) {
            tokenCharsArg = std::string("tokenchars=") + _tokenChars;
            argv[argc++] = tokenCharsArg.c_str();
        }
        sqlite3_tokenizer* tokenizer;
        int err = sModule->xCreate(argc, argv, &tokenizer);
        return err ? NULL : tokenizer;
    }

    sqlite3_tokenizer* Tokenizer::getTokenizer() {
        if (!_tokenizer)
            _tokenizer = createTokenizer();
        return _tokenizer;
    }

    const word_set& Tokenizer::stopwords() const {
        return sLanguageToStopwords[_language];
    }


#pragma mark TOKENITERATOR:


    TokenIterator::TokenIterator(Tokenizer &tokenizer, slice text, bool unique)
    :_stopwords(tokenizer.stopwords()),
     _unique(unique)
    {
        int err = sModule->xOpen(tokenizer.getTokenizer(), (const char*)text.buf, (int)text.size,
                                 &_cursor);
        assert(!err);
        _cursor->pTokenizer = tokenizer.getTokenizer(); // module expects sqlite3 to have initialized this
        next(); // advance to 1st token
    }

    TokenIterator::~TokenIterator() {
        sModule->xClose(_cursor);
    }

    bool TokenIterator::next() {
        for (;;) {
            const char *tokenBytes;
            int tokenLength;
            int startOffset, endOffset, pos;
            int err = sModule->xNext(_cursor, &tokenBytes, &tokenLength,
                                     &startOffset, &endOffset, &pos);
            _hasToken = (err == SQLITE_OK);
            if (!_hasToken)
                return false;
            _token = std::string(tokenBytes, tokenLength);
            if (_stopwords.count(_token) > 0)
                continue; // it's a stop-word
            if (_unique) {
                auto result = _seen.emplace(_token, true);
                if (!result.second)
                    continue; // already seen this token, go on to next one
            }
            _wordOffset = startOffset;
            _wordLength = endOffset - startOffset;
            return true;
        }
    }

}