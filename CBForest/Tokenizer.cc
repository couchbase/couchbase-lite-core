//
//  Tokenizer.cc
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
#include "LogInternal.hh"
#include "Error.hh"

#ifndef __unused
#define __unused
#endif

#ifdef _MSC_VER
#include "memmem.h"
#endif

extern "C" {
    #include "fts3_tokenizer.h"
    #include "fts3_unicodesn.h"
}

namespace cbforest {

    static const struct sqlite3_tokenizer_module* sModule;
    static std::unordered_map<std::string, word_set> sStemmerToStopwords;

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

    std::string Tokenizer::defaultStemmer;
    bool Tokenizer::defaultRemoveDiacritics = false;

    Tokenizer::Tokenizer(std::string stemmer, bool removeDiacritics)
    :_stemmer(stemmer),
     _removeDiacritics(removeDiacritics),
     _tokenizer(NULL),
     _tokenChars("'’")
    {
        if (!sModule) { //FIX: Make this thread-safe
            sqlite3Fts3UnicodeSnTokenizer(&sModule);
            sStemmerToStopwords["en"] = sStemmerToStopwords["english"] =
                                                                readWordList(kEnglishStopWords);
        }
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
        if (err) {
            Warn("Couldn't create tokenizer: err=%d", err);
            tokenizer = NULL;
        }
        return tokenizer;
    }

    sqlite3_tokenizer* Tokenizer::getTokenizer() {
        if (!_tokenizer)
            _tokenizer = createTokenizer();
        return _tokenizer;
    }

    const word_set& Tokenizer::stopwords() const {
        return sStemmerToStopwords[_stemmer];
    }


#pragma mark TOKENITERATOR:


    static void trimQuotes(const char* &token, int &length);
    static bool isCurly(slice);
    static std::string uncurl(std::string token);


    TokenIterator::TokenIterator(Tokenizer &tokenizer, slice text, bool unique)
    :_stopwords(tokenizer.stopwords()),
     _unique(unique)
    {
        if (isCurly(text)) {
            // Need to copy the input text in order to convert curly close quotes to apostrophes:
            _text = uncurl((std::string)text);
            text = _text;
        }

        auto tok = tokenizer.getTokenizer();
        if (!tok)
            throw error(error::TokenizerError);
        __unused int err = sModule->xOpen(tok, (const char*)text.buf, (int)text.size, &_cursor);
        CBFAssert(!err);
        _cursor->pTokenizer = tok; // module expects sqlite3 to have initialized this
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
            trimQuotes(tokenBytes, tokenLength);
            if (tokenLength == 0)
                continue;
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


    // Trim apostrophes (since we told the tokenizer they're alphabetical)
    static void trimQuotes(const char* &token, int &length) {
        while (length > 0) {
            if (token[length-1] == '\'') {
                --length;
            } else if (token[0] == '\'') {
                ++token;
                --length;
            } else if (length >= 3 && memcmp(token+length-3, "’", 3)==0) {
                length -= 3;
            } else if (length >= 3 && memcmp(token, "’", 3)==0) {
                token += 3;
                length -= 3;
            } else {
                break; // done
            }
        }
    }

    static bool isCurly(slice text) {
        return memmem(text.buf, text.size, "’", 3) != NULL;
    }

    // Convert curly-close-quote to straight apostrophe
    static std::string uncurl(std::string token) {
        while(true) {
            size_t pos = token.find("’");
            if (pos == std::string::npos)
                break;
            token = token.replace(pos, 3, "'");
        }
        return token;
    }

}
