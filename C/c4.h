//
//  c4.h
//  CBForest
//
//  Basic types and functions for C API.
//
//  Created by Jens Alfke on 9/8/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef c4_h
#define c4_h

#include <stdbool.h>
#include <stdint.h>


/** An error value. These are returned by reference from API calls whose last parameter is a
    C4Error*. The semantics are based on Cocoa's usage of NSError:
    * A caller can pass NULL if it doesn't care about the error.
    * The error is filled in only if the function fails, as indicated by its return value
      (e.g. false or NULL.) If the function doesn't fail, it does NOT zero out the error, so its
      contents should be considered uninitialized garbage. */
typedef struct {
    enum {
        HTTPDomain,         // code is an HTTP status code
        POSIXDomain,        // code is an errno
        ForestDBDomain,     // code is a fdb_status
        C4Domain            // code is C4-specific code (TBD)
    } domain;
    int32_t code;
} C4Error;


#ifndef C4_IMPL

/** A slice is simply a pointer to a range of bytes, usually interpreted as a UTF-8 string.
    A "null slice" has chars==NULL and length==0.
    A slice with length==0 is not necessarily null; if chars!=NULL it's an empty string.
    A slice as a function parameter is read-only: the function will not alter or free the bytes.
    A slice _returned from_ a function points to newly-allocated memory and must be freed via
    c4slice_free. */
typedef struct {
    const void *buf;
    size_t size;
} C4Slice;

/** A slice returned from a function needs to have its buf freed by the caller. */
typedef C4Slice C4SliceResult;

// A convenient constant denoting a null slice.
#define kC4SliceNull ((C4Slice){NULL, 0})

#endif

/** Frees the memory of a heap-allocated slice by calling free(chars). */
void c4slice_free(C4Slice);


/** A database sequence number, representing the order in which a revision was created. */
typedef uint64_t C4SequenceNumber;


#endif /* c4_h */
