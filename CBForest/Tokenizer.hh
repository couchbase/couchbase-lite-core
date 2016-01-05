//
//  Tokenizer.hh
//  CBForest
//
//  Created by Jens Alfke on 10/17/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__Tokenizer__
#define __CBForest__Tokenizer__

#include "slice.hh"
#include <unordered_map>

struct sqlite3_tokenizer;
struct sqlite3_tokenizer_cursor;

namespace cbforest {

    class TokenIterator;
    typedef std::unordered_map<std::string, bool> word_set;

    /** A Tokenizer manages tokenization of strings. An instance is configured with a specific
        language and can then generate TokenIterators from strings. */
    class Tokenizer {
    public:
        static std::string defaultStemmer;
        static bool defaultRemoveDiacritics;

        /** Initializes a Tokenizer.
            @param stemmer  The name of a stemmer (e.g. "english") or an empty string for language-
                        neutral tokenization.
            @param removeDiacritics  True if diacritical marks (accents) should be stripped from
                        the input text. */
        Tokenizer(std::string stemmer = defaultStemmer,
                  bool removeDiacritics = defaultRemoveDiacritics);

        ~Tokenizer();

        const std::string& stemmer()        {return _stemmer;}

        /** Defines extra characters that should be considered part of a token. */
        void setTokenChars(std::string s)   {_tokenChars = s;}
        std::string tokenChars() const      {return _tokenChars;}

    private:
        sqlite3_tokenizer* createTokenizer();
        sqlite3_tokenizer* getTokenizer();
        const word_set &stopwords() const;

        std::string _stemmer;
        bool _removeDiacritics;
        struct sqlite3_tokenizer* _tokenizer;
        std::string _tokenChars;
        friend class TokenIterator;
    };

    /** Iterates over the word tokens found in a string, as defined by a Tokenizer. */
    class TokenIterator {
    public:
        /** Initializes a TokenIterator that will tokenize the given string.
            @param tokenizer  The tokenizer to use.
            @param text  The input text, as UTF-8 data.
            @param unique  If true, only unique tokens will be returned. */
        TokenIterator(Tokenizer& tokenizer, slice text, bool unique =false);
        ~TokenIterator();

        /** True if the iterator has a token, false if it's reached the end. */
        bool hasToken() const           {return _hasToken;}
        /** The current token. */
        std::string token() const       {return _token;}
        /** The byte offset in the input string where the tokenized word begins. */
        size_t wordOffset() const      {return _wordOffset;}
        /** The length in bytes of the tokenized word.
            (Will often be longer than the length of the token string due to stemming.) */
        size_t wordLength() const       {return _wordLength;}

        /** Finds the next token, returning false when it reaches the end. */
        bool next();

        operator bool() const           {return hasToken();}
        TokenIterator& operator++()     {next(); return *this;}

    private:
        friend class Tokenizer;
        TokenIterator(sqlite3_tokenizer_cursor*, const word_set&, bool unique);

        std::string _text;
        sqlite3_tokenizer_cursor* _cursor;
        const word_set &_stopwords;
        const bool _unique;
        std::unordered_map<std::string, bool> _seen;
        bool _hasToken;
        std::string _token;
        size_t _wordOffset, _wordLength;
    };

}

#endif /* defined(__CBForest__Tokenizer__) */
