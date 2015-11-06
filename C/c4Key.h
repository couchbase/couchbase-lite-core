//
//  c4Key.h
//  CBForest
//
//  Created by Jens Alfke on 11/6/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef c4Key_h
#define c4Key_h

#ifdef __cplusplus
extern "C" {
#endif

    //////// KEYS:


    /** An opaque value used as a key or value in a view index. The data types that can be stored
        in a C4Key are the same as JSON, but the actual data format is quite different. */
    typedef struct c4Key C4Key;

    /** Creates a new empty C4Key. */
    C4Key* c4key_new();

    /** Creates a C4Key by copying the data, which must be in the C4Key binary format. */
    C4Key* c4key_withBytes(C4Slice);

    /** Frees a C4Key. */
    void c4key_free(C4Key*);

    void c4key_addNull(C4Key*);             /**< Adds a JSON null value to a C4Key. */
    void c4key_addBool(C4Key*, bool);       /**< Adds a boolean value to a C4Key. */
    void c4key_addNumber(C4Key*, double);   /**< Adds a number to a C4Key. */
    void c4key_addString(C4Key*, C4Slice);  /**< Adds a string to a C4Key. */

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

#ifdef __cplusplus
    }
#endif

#endif /* c4Key_h */
