//
// c4Compat.hh
//
// Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once


#ifndef __has_feature
#define __has_feature(x) 0
#endif
#ifndef __has_attribute
#define __has_attribute(x) 0
#endif
#ifndef __has_extension
#define __has_extension(x) 0
#endif

#ifdef _MSC_VER
#define C4INLINE __forceinline
#define C4NONNULL
#else
#define C4INLINE inline
#define C4NONNULL __attribute((nonnull))
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
#elif DOXYGEN_PARSING
    #define C4_ENUM(_type, _name)     enum _name : _type _name; enum _name : _type
    #define C4_OPTIONS(_type, _name) enum _name : _type _name; enum _name : _type
#else
    #if (__cplusplus && _MSC_VER) || (__cplusplus && __cplusplus >= 201103L && (__has_extension(cxx_strong_enums) || __has_feature(objc_fixed_enum))) || (!__cplusplus && __has_feature(objc_fixed_enum))
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
#define C4API noexcept
#else
#define C4API
#endif


// Export/import stuff:
#ifdef _MSC_VER
#ifdef LITECORE_EXPORTS
#define CBL_CORE_API __declspec(dllexport)
#else
#define CBL_CORE_API __declspec(dllimport)
#endif
#else // _MSC_VER
#define CBL_CORE_API
#endif

// Type-checking for printf-style vararg functions:
#ifdef _MSC_VER
#define __printflike(A, B)
#else
#ifndef __printflike
#define __printflike(fmtarg, firstvararg) __attribute__((__format__ (__printf__, fmtarg, firstvararg)))
#endif
#endif
