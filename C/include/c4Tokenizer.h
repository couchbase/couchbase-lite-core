//
// c4Tokenizer.h
//
// Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once
#include "c4Base.h"

#ifdef __cplusplus
extern "C" {
#endif


/** \defgroup Tokenizer Custom Tokenizers For Full-Text Search
    @{ */


/** A custom text tokenizer for use with full-text search.
    Its responsibility is to create C4TokenizerCursor objects.
    This struct is allocated and initialized by the client, and returned from the
    C4TokenizerFactory callback. */
typedef struct {
    const struct C4TokenizerMethods* methods;
} C4Tokenizer;


/** A custom text tokenizer cursor for use with full-text search.
    Its responsibility is to take an input string (UTF-8) and, on every call to its `next`
    method, return the next token (word) in the input.
    This struct is allocated and initialized by the client, and returned from the
    C4TokenizerMethods.newCursor callback. */
typedef struct {
    const struct C4TokenizerCursorMethods* methods;
} C4TokenizerCursor;


/** Struct defining the methods that can be called on a C4Tokenizer, as C callbacks. */
typedef struct C4TokenizerMethods {
    /** Must allocate a new C4TokenizerCursor and initialize its `methods` pointer.
        Since the cursor also needs to remember the input text, and keep track of how far it's
        read, you'll probably want to allocate a larger block and store your own state after
        the C4TokenizerCursor fields. */
    C4TokenizerCursor* (*newCursor)(C4Tokenizer* self,
                                    C4String inputText,
                                    C4Error*);
    /** Must free the C4Tokenizer and any other resources it allocated.
        If this callback is left NULL, the default is to call `free()`. */
    void (*free)(C4Tokenizer* self);
} C4TokenizerMethods;


/** Struct defining the methods that can be called on a C4TokenizerCursor, as C callbacks. */
typedef struct C4TokenizerCursorMethods {
    /** Must read the next token (word) from the cursor's input text, or return false at the end.
        @param self  The cursor. You can use this pointer to access extra state.
        @param outToken  On return, should point to the token in normalized form, e.g. with case
            distinctions removed, possibly diacritics removed, and possibly "stemmed". The memory
            for this must be managed by the cursor, but it only needs to remain valid until the
            next call to `next`, so it can just be a simple buffer in the object.
        @param outTokenRange  The range in the input text occupied by the token. Must point into
            the original `inputText` given when the cursor was created.
        @param outError  If an error occurs, store it here and return false.
        @return  True if a token was read, false at end of text or upon error. */
    bool (*next)(C4TokenizerCursor* self,
                 C4String* outToken,
                 C4String* outTokenRange,
                 C4Error *outError);
    /** Must free the C4TokenizerCursor and any other resources it allocated.
        If this callback is left NULL, the default is to call `free()`. */
    void (*free)(C4TokenizerCursor* self);
} C4TokenizerCursorMethods;


/** Factory function that must allocate a C4Tokenizer, set its `methods` pointer, and return it. */
typedef C4Tokenizer* (*C4TokenizerFactory)(const C4IndexOptions*);


/** Registers a text tokenizer with LiteCore.
    This function can only be called once. */
void c4query_setFTSTokenizerFactory(C4TokenizerFactory) C4API;


/** @} */

#ifdef __cplusplus
}
#endif
