//
// c4IndexTypes.h
//
// Copyright (c) 2019 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "c4Base.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS


/** \defgroup Indexing  Database Indexes
 @{ */


/** Types of indexes. */
typedef C4_ENUM(uint32_t, C4IndexType) {
    kC4ValueIndex,         ///< Regular index of property value
    kC4FullTextIndex,      ///< Full-text index
    kC4ArrayIndex,         ///< Index of array values, for use with UNNEST
    kC4PredictiveIndex,    ///< Index of prediction() results (Enterprise Edition only)
};


/** Options for indexes; these each apply to specific types of indexes. */
typedef struct {
    /** Dominant language of text to be indexed; setting this enables word stemming, i.e.
        matching different cases of the same word ("big" and "bigger", for instance.)
        Can be an ISO-639 language code or a lowercase (English) language name; supported
        languages are: da/danish, nl/dutch, en/english, fi/finnish, fr/french, de/german,
        hu/hungarian, it/italian, no/norwegian, pt/portuguese, ro/romanian, ru/russian,
        es/spanish, sv/swedish, tr/turkish.
        If left null,  or set to an unrecognized language, no language-specific behaviors
        such as stemming and stop-word removal occur. */
    const char* C4NULLABLE language;

    /** Should diacritical marks (accents) be ignored? Defaults to false.
        Generally this should be left false for non-English text. */
    bool ignoreDiacritics;

    /** "Stemming" coalesces different grammatical forms of the same word ("big" and "bigger",
        for instance.) Full-text search normally uses stemming if the language is one for
        which stemming rules are available, but this flag can be set to `true` to disable it.
        Stemming is currently available for these languages: da/danish, nl/dutch, en/english,
        fi/finnish, fr/french, de/german, hu/hungarian, it/italian, no/norwegian, pt/portuguese,
        ro/romanian, ru/russian, s/spanish, sv/swedish, tr/turkish. */
    bool disableStemming;

    /** List of words to ignore ("stop words") for full-text search. Ignoring common words
        like "the" and "a" helps keep down the size of the index.
        If NULL, a default word list will be used based on the `language` option, if there is
        one for that language.
        To suppress stop-words, use an empty string.
        To provide a custom list of words, use a string containing the words in lowercase
        separated by spaces. */
    const char* C4NULLABLE stopWords;
} C4IndexOptions;


/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
