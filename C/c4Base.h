//
//  c4Base.h
//  CBForest
//
//  Basic types and functions for C API.
//
//  Created by Jens Alfke on 9/8/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef c4Base_h
#define c4Base_h

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef _MSC_VER
#define __has_extension
#define __has_feature
#define CBINLINE __forceinline
#else
#define CBINLINE inline
#endif

// Macros for defining typed enumerations and option flags.
// To define an enumeration whose values won't be combined:
//      typedef C4_ENUM(baseIntType, name) { ... };
// To define an enumeration of option flags that will be ORed together:
//      typedef C4_OPTIONS(baseIntType, name) { ... };
// These aren't just a convenience; they are required for Swift bindings.
#if APPLE
    #include <CoreFoundation/CFBase.h>      /* for CF_ENUM and CF_OPTIONS macros */
    #define C4_ENUM CF_ENUM
    #define C4_OPTIONS CF_OPTIONS
#else
    #if (__cplusplus && __cplusplus >= 201103L && (__has_extension(cxx_strong_enums) || __has_feature(objc_fixed_enum))) || (!__cplusplus && __has_feature(objc_fixed_enum))
        #define C4_ENUM(_type, _name)     enum _name : _type _name; enum _name : _type
        #if (__cplusplus)
            #define C4_OPTIONS(_type, _name) _type _name; enum : _type
        #else
            #define C4_OPTIONS(_type, _name) enum _name : _type _name; enum _name : _type
        #endif
    #else
        #define C4_ENUM(_type, _name) _type _name; enum
        #define C4_OPTIONS(_type, _name) _type _name; enum
    #endif
#endif


#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER
#ifdef CBFOREST_EXPORTS
#define CBFOREST_API __declspec(dllexport)
#else
#define CBFOREST_API __declspec(dllimport)
#endif
#else // _MSC_VER
#define CBFOREST_API
#endif


/** A database sequence number, representing the order in which a revision was created. */
typedef uint64_t C4SequenceNumber;


//////// ERRORS:


typedef C4_ENUM(uint32_t, C4ErrorDomain) {
    HTTPDomain,         // code is an HTTP status code
    POSIXDomain,        // code is an errno
    ForestDBDomain,     // code is a fdb_status
    C4Domain            // code is C4-specific (see below)
};


// HTTPDomain error codes:
enum {
    kC4HTTPBadRequest   = 400,          // Invalid parameters
    kC4HTTPNotFound     = 404,          // Doc/revision not found
    kC4HTTPConflict     = 409,          // Doc update conflict (parent rev no longer current)
    kC4HTTPGone         = 410,          // Rev body has been compacted away
};


// C4Domain error codes:
enum {
    kC4ErrorInternalException = 1,      // CBForest threw an unexpected C++ exception
    kC4ErrorNotInTransaction,           // Function must be called while in a transaction
    kC4ErrorTransactionNotClosed,       // Database can't be closed while a transaction is open
    kC4ErrorIndexBusy,                  // View can't be closed while index is enumerating

    // These come from CBForest (error.hh)
    kC4ErrorBadRevisionID = -1000,
    kC4ErrorCorruptRevisionData = -1001,
    kC4ErrorCorruptIndexData = -1002,
    kC4ErrorAssertionFailed = -1003,
    kC4ErrorTokenizerError = -1004,     // can't create FTS tokenizer

};


/** An error value. These are returned by reference from API calls whose last parameter is a
    C4Error*. The semantics are based on Cocoa's usage of NSError:
    * A caller can pass NULL if it doesn't care about the error.
    * The error is filled in only if the function fails, as indicated by its return value
      (e.g. false or NULL.) If the function doesn't fail, it does NOT zero out the error, so its
      contents should be considered uninitialized garbage. */
typedef struct {
    C4ErrorDomain domain;
    int32_t code;
} C4Error;


//////// SLICES:


#ifndef C4_IMPL

/** A slice is simply a pointer to a range of bytes, usually interpreted as a UTF-8 string.
    A "null slice" has chars==NULL and length==0.
    A slice with length==0 is not necessarily null; if chars!=NULL it's an empty string.
    A slice as a function parameter is temporary and read-only: the function will not alter or free
    the bytes, and the pointer won't be accessed after the function returns.
    A slice _returned from_ a function points to newly-allocated memory and must be freed by the
    caller, with c4slice_free(). */
typedef struct {
    const void *buf;
    size_t size;
} C4Slice;

/** Denotes a slice returned from a function, which needs to have its buf freed by the caller. */
typedef C4Slice C4SliceResult;

/** Creates a slice pointing to the contents of a C string. */
static CBINLINE C4Slice c4str(const char *str) {
	C4Slice foo = { str, str ? strlen(str) : 0 };
	return foo;
}

// Macro version of c4str, for use in initializing compile-time constants.
// STR must be a C string literal.
#ifdef _MSC_VER
#define C4STR(STR) {("" STR), sizeof(("" STR))-1}
#else
#define C4STR(STR) ((C4Slice){("" STR), sizeof(("" STR))-1})
#endif

// A convenient constant denoting a null slice.
#ifdef _MSC_VER
const C4Slice kC4SliceNull = { NULL, 0 };
#else
#define kC4SliceNull ((C4Slice){NULL, 0})
#endif

#endif // C4_IMPL

/** Returns true if two slices have equal contents. */
bool c4SliceEqual(C4Slice a, C4Slice b);

/** Frees the memory of a heap-allocated slice by calling free(buf). */
void c4slice_free(C4Slice);


/** Logging levels. */
typedef C4_ENUM(uint8_t, C4LogLevel) {
    kC4LogDebug,
    kC4LogInfo,
    kC4LogWarning,
    kC4LogError
};

/** A logging callback that the application can register. */
typedef void (*C4LogCallback)(C4LogLevel level, C4Slice message);

/** Registers (or unregisters) a log callback, and sets the minimum log level to report.
    Before this is called, logs are by default written to stderr for warnings and errors.
    Note that this setting is global to the entire process.
    @param level  The minimum level of message to log.
    @param callback  The logging callback, or NULL to disable logging entirely. */
void c4log_register(C4LogLevel level, C4LogCallback callback);


#ifdef __cplusplus
}
#endif

#endif /* c4Base_h */
