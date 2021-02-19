//
// c4Internal.hh
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
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

#include "Base.hh"
#include "Error.hh"
#include "RefCounted.hh"
#include "PlatformCompat.hh"
#include "fleece/Fleece.h"
#include "c4Base.h"
#include "c4ExceptionUtils.hh"
#include <mutex>
#include <string_view>

using namespace litecore;

#define LOCK(MUTEX)     std::unique_lock<std::mutex> _lock(MUTEX)
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


namespace c4Internal {

    // ERRORS & EXCEPTIONS:

#if DEBUG
    static constexpr size_t kMaxErrorMessagesToSave = 100;
#else
    static constexpr size_t kMaxErrorMessagesToSave = 10;
#endif

    // SLICES:

    C4SliceResult sliceResult(const char *str);
    C4SliceResult sliceResult(const std::string&);

    std::string toString(C4Slice);

    void destructExtraInfo(C4ExtraInfo&) noexcept;
}

using namespace c4Internal;
