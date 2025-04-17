//
// c4Compat.h
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/CompilerSupport.h"

#ifdef _MSC_VER
#    define C4INLINE __forceinline
#else
#    define C4INLINE inline
#endif

// Non-null annotations, for function parameters and struct fields.
// In between C4_ASSUME_NONNULL_BEGIN and C4_ASSUME_NONNULL_END, all pointer declarations implicitly
// disallow NULL values, unless annotated with C4NULLABLE (which must come after the `*`.)
// (C4NONNULL is occasionally necessary when there are multiple levels of pointers.)
// NOTE: Does not apply to function return values, for some reason. Those may still be null,
//       unless annotated with C4_RETURNS_NONNULL.
// NOTE: Only supported in Clang, so far.
#if __has_feature(nullability)
#    define C4_ASSUME_NONNULL_BEGIN _Pragma("clang assume_nonnull begin")
#    define C4_ASSUME_NONNULL_END   _Pragma("clang assume_nonnull end")
#    define C4NULLABLE              _Nullable
#    define C4NONNULL               _Nonnull
#    define C4_RETURNS_NONNULL      __attribute__((returns_nonnull))
#else
#    define C4_ASSUME_NONNULL_BEGIN
#    define C4_ASSUME_NONNULL_END
#    define C4NULLABLE
#    define C4NONNULL
#    define C4_RETURNS_NONNULL
#endif

// Macros for defining typed enumerations and option flags.
// To define an enumeration whose values won't be combined:
//      typedef C4_ENUM(baseIntType, name) { ... };
// To define an enumeration of option flags that will be ORed together:
//      typedef C4_OPTIONS(baseIntType, name) { ... };
// These aren't just a convenience; they are required for Swift bindings.
#if __has_attribute(enum_extensibility)
#    define C4_ENUM_ATTRIBUTES    __attribute__((enum_extensibility(open)))
#    define C4_OPTIONS_ATTRIBUTES __attribute__((flag_enum, enum_extensibility(open)))
#else
#    define C4_ENUM_ATTRIBUTES
#    define C4_OPTIONS_ATTRIBUTES
#endif

#if __APPLE__
#    include <CoreFoundation/CFBase.h> /* for CF_ENUM and CF_OPTIONS macros */
#    define C4_ENUM    CF_ENUM
#    define C4_OPTIONS CF_OPTIONS
#elif DOXYGEN_PARSING
#    define C4_ENUM(_type, _name)                                                                                      \
        enum _name : _type _name;                                                                                      \
        enum _name : _type
#    define C4_OPTIONS(_type, _name)                                                                                   \
        enum _name : _type _name;                                                                                      \
        enum _name : _type
#else
#    if ( __cplusplus && _MSC_VER )                                                                                    \
            || (__cplusplus && __cplusplus >= 201103L                                                                  \
                && (__has_extension(cxx_strong_enums) || __has_feature(objc_fixed_enum)))                              \
            || (!__cplusplus && __has_feature(objc_fixed_enum))
#        define C4_ENUM(_type, _name)                                                                                  \
            int                     __C4_ENUM_##_name;                                                                 \
            enum C4_ENUM_ATTRIBUTES _name : _type;                                                                     \
            typedef enum _name      _name;                                                                             \
            enum _name : _type
#        if ( __cplusplus )
#            define C4_OPTIONS(_type, _name)                                                                           \
                _type _name;                                                                                           \
                enum C4_OPTIONS_ATTRIBUTES : _type
#        else
#            define C4_OPTIONS(_type, _name)                                                                           \
                int                        __C4_OPTIONS_##_name;                                                       \
                enum C4_OPTIONS_ATTRIBUTES _name : _type;                                                              \
                typedef enum _name         _name;                                                                      \
                enum _name : _type
#        endif
#    else
#        define C4_ENUM(_type, _name)                                                                                  \
            _type _name;                                                                                               \
            enum
#        define C4_OPTIONS(_type, _name)                                                                               \
            _type _name;                                                                                               \
            enum
#    endif
#endif


// Declaration for API functions; should be just before the ending ";".
#ifdef __cplusplus
#    define C4API             noexcept
#    define C4API_BEGIN_DECLS extern "C" {
#    define C4API_END_DECLS   }
#else
#    define C4API
#    define C4API_BEGIN_DECLS
#    define C4API_END_DECLS
#endif

// Deprecating functions & types  (Note: In C++only code, can use standard `[[deprecated]]`)
#ifdef _MSC_VER
#    define C4_DEPRECATED(MSG) __declspec(deprecated(MSG))
#else
#    define C4_DEPRECATED(MSG) __attribute((deprecated(MSG)))
#endif

// Export/import stuff.
// `CBL_CORE_API` goes before an `extern` declaration,
// `CBL_CORE_API_IMPL` goes before the definition.
#ifdef _MSC_VER
#    ifdef LITECORE_EXPORTS
#        define CBL_CORE_API __declspec(dllexport)
#    else
#        define CBL_CORE_API __declspec(dllimport)
#    endif
#    define CBL_CORE_API_IMPL CBL_CORE_API
#else
#    define CBL_CORE_API __attribute__((visibility("default")))
#    ifdef __clang__
#        define CBL_CORE_API_IMPL CBL_CORE_API
#    else
#        define CBL_CORE_API_IMPL
#    endif
#endif


// Type-checking for printf-style vararg functions:
#ifdef _MSC_VER
#    define __printflike(A, B)
#else
#    ifndef __printflike
#        define __printflike(fmtarg, firstvararg) __attribute__((__format__(__printf__, fmtarg, firstvararg)))
#    endif
#endif
