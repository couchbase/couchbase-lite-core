//
// c4Internal.hh
//
// Copyright 2015-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.h"


#define LOCK(MUTEX)     std::unique_lock<decltype(MUTEX)> _lock(MUTEX)
#define UNLOCK()        _lock.unlock();

#if defined(__clang__)
    #define C4_START_WARNINGS_SUPPRESSION _Pragma( "clang diagnostic push" )
    #define C4_STOP_WARNINGS_SUPPRESSION _Pragma( "clang diagnostic pop" )
    #define C4_IGNORE_TAUTOLOGICAL _Pragma( "clang diagnostic ignored \"-Wtautological-pointer-compare\"" )
    #define C4_IGNORE_NONNULL _Pragma( "clang diagnostic ignored \"-Wnonnull\"" )
#elif defined(__GNUC__)
    #define C4_START_WARNINGS_SUPPRESSION _Pragma( "GCC diagnostic push" )
    #define C4_STOP_WARNINGS_SUPPRESSION _Pragma( "GCC diagnostic pop" )
    #define C4_IGNORE_TAUTOLOGICAL
    #define C4_IGNORE_NONNULL _Pragma( "GCC diagnostic ignored \"-Wnonnull\"" )
#elif defined(_MSC_VER)
    #define C4_START_WARNINGS_SUPPRESSION __pragma( warning(push) )
    #define C4_STOP_WARNINGS_SUPPRESSION __pragma( warning(pop) )
    #define C4_IGNORE_TAUTOLOGICAL
    #define C4_IGNORE_NONNULL
#else
    #define C4_START_WARNINGS_SUPPRESSION
    #define C4_STOP_WARNINGS_SUPPRESSION
    #define C4_IGNORE_TAUTOLOGICAL
    #define C4_IGNORE_NONNULL
#endif


struct C4ExtraInfo;

namespace litecore {

    // ERRORS & EXCEPTIONS:

#if DEBUG
    static constexpr size_t kMaxErrorMessagesToSave = 100;
#else
    static constexpr size_t kMaxErrorMessagesToSave = 10;
#endif

    // UTILITIES:

    C4SliceResult toSliceResult(const std::string&);

    void destructExtraInfo(C4ExtraInfo&) noexcept;

}
