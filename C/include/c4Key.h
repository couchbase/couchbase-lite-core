//
//  c4Key.h
//  CBForest
//
//  Created by Jens Alfke on 11/6/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef c4Key_h
#define c4Key_h
#include "c4Base.h"

#ifdef __cplusplus
extern "C" {
#endif

    // Language code denoting "the default language"
    #define kC4LanguageDefault  kC4SliceNull

    // Language code denoting "no language"
    #define kC4LanguageNone     C4STR("")

    /** A 2D bounding box used for geo queries */
    typedef struct {
        double xmin, ymin, xmax, ymax;
    } C4GeoArea;


    //////// KEYS:


    /** An opaque value used as a key or value in a view index. The data types that can be stored
        in a C4Key are the same as JSON, but the actual data format is quite different. */
    typedef struct c4Key C4Key;

    /** Creates a new empty C4Key. */
    C4Key* c4key_new(void);

    /** Creates a C4Key by copying the data, which must be in the C4Key binary format. */
    C4Key* c4key_withBytes(C4Slice);

    /** Creates a C4Key containing a string of text to be full-text-indexed by a view.
        @param text  The text to be indexed.
        @param language  The human language of the string as an ISO-639 code like "en";
                    or kC4LanguageNone to disable language-specific transformations such as
                    stemming; or kC4LanguageDefault to fall back to the default language
                    (as set by c4key_setDefaultFullTextLanguage.)
        @return  A new C4Key representing this key. */
    C4Key* c4key_newFullTextString(C4Slice text, C4Slice language);

    /** Creates a C4Key containing a 2D shape to be geo-indexed.
        Caller must provide a bounding box (which is what's actually used for searching.)
        @param geoJSON  GeoJSON describing the shape.
        @param boundingBox  A conservative bounding box of the shape.
        @return  A new C4Key for the shape. */
    C4Key* c4key_newGeoJSON(C4Slice geoJSON, C4GeoArea boundingBox);

    /** Frees a C4Key. */
    void c4key_free(C4Key*);

    void c4key_addNull(C4Key*);             /**< Adds a JSON null value to a C4Key. */
    void c4key_addBool(C4Key*, bool);       /**< Adds a boolean value to a C4Key. */
    void c4key_addNumber(C4Key*, double);   /**< Adds a number to a C4Key. */
    void c4key_addString(C4Key*, C4Slice);  /**< Adds a UTF-8 string to a C4Key. */

    /** Adds an array to a C4Key.
        Subsequent values added will go into the array, until c4key_endArray is called. */
    void c4key_beginArray(C4Key*);

    /** Closes an array opened by c4key_beginArray. (Every array must be closed.) */
    void c4key_endArray(C4Key*);

    /** Adds a map/dictionary/object to a C4Key.
        Subsequent keys and values added will go into the map, until c4key_endMap is called. */
    void c4key_beginMap(C4Key*);

    /** Closes a map opened by c4key_beginMap. (Every map must be closed.) */
    void c4key_endMap(C4Key*);

    /** Adds a map key, before the next value. When adding to a map, every value must be
        preceded by a key. */
    void c4key_addMapKey(C4Key*, C4Slice);


    /** Sets the process-wide default (human) language for full-text keys. This affects how
        words are "stemmed" (stripped of suffixes like "-ing" or "-est" in English) when indexed.
        @param languageName  An ISO language name like 'english'.
        @param stripDiacriticals  True if accents and other diacriticals should be stripped from
                                  letters. Appropriate for English but not for most other languages.
        @return  True if the languageName was recognized, false if not. */
    bool c4key_setDefaultFullTextLanguage(C4Slice languageName, bool stripDiacriticals);


    //////// KEY READERS:


    /** A struct pointing to the raw data of an encoded key. The functions that operate
        on this allow it to be parsed by reading items one at a time (similar to SAX parsing.) */
    typedef struct {
        const void *bytes;
        size_t length;
    } C4KeyReader;

    /** The types of tokens in a key. */
    typedef C4_ENUM(uint8_t, C4KeyToken) {
        kC4Null,
        kC4Bool,
        kC4Number,
        kC4String,
        kC4Array,
        kC4Map,
        kC4EndSequence,
        kC4Special,
        kC4Error = 255
    };


    /** Returns a C4KeyReader that can parse the contents of a C4Key.
        Warning: Adding to the C4Key will invalidate the reader. */
    C4KeyReader c4key_read(const C4Key *key);

    /** for java binding */
    C4KeyReader* c4key_newReader(const C4Key *key);

    /** Free a C4KeyReader */
    void c4key_freeReader(C4KeyReader*);

    /** Returns the type of the next item in the key, or kC4Error at the end of the key or if the
        data is corrupt.
        To move on to the next item, you must call skipToken or one of the read___ functions. */
    C4KeyToken c4key_peek(const C4KeyReader*);

    /** Skips the current token in the key. If it was kC4Array or kC4Map, the reader will
        now be positioned at the first item of the collection. */
    void c4key_skipToken(C4KeyReader*);

    bool c4key_readBool(C4KeyReader*);              /**< Reads a boolean value. */
    double c4key_readNumber(C4KeyReader*);          /**< Reads a numeric value. */
    C4SliceResult c4key_readString(C4KeyReader*);   /**< Reads a string (remember to free it!) */

    /** Converts a C4KeyReader to JSON. Remember to free the result. */
    C4SliceResult c4key_toJSON(const C4KeyReader*);


    //////// KEY/VALUE LISTS:


    /** An opaque list of key/value pairs, used when indexing a view. */
    typedef struct c4KeyValueList C4KeyValueList;

    /** Creates a new empty list. */
    C4KeyValueList* c4kv_new(void);

    /** Adds a key/value pair to a list. The key and value are copied. */
    void c4kv_add(C4KeyValueList *kv, C4Key *key, C4Slice value);

    /** Removes all keys and values from a list. */
    void c4kv_reset(C4KeyValueList *kv);

    /** Frees all storage used by a list (including its copied keys and values.) */
    void c4kv_free(C4KeyValueList *kv);

#ifdef __cplusplus
    }
#endif

#endif /* c4Key_h */
