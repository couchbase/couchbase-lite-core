//
//  Instrumentation.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Instrumentation.hh"

#ifdef __APPLE__
#include <sys/kdebug_signpost.h>
#include <os/signpost.h>
#endif

namespace litecore {

#if defined(__APPLE__) && LITECORE_SIGNPOSTS

#if TARGET_OS_MACCATALYST || __MAC_OS_X_VERSION_MAX_ALLOWED >= 101500 || __IPHONE_OS_VERSION_MIN_REQUIRED >= 130000
    os_log_t LiteCore = os_log_create("com.couchbase.litecore", "signposts");;
#endif

    enum Color {
        blue, green, purple, orange, red    // used for last argument
    };

    void Signpost::mark(Type t, uintptr_t param, uintptr_t param2) {
#if TARGET_OS_MACCATALYST || __MAC_OS_X_VERSION_MAX_ALLOWED >= 101500 || __IPHONE_OS_VERSION_MIN_REQUIRED >= 130000
        if (__builtin_available(iOS 12.0, macOS 10.14, *))
            os_signpost_event_emit(LiteCore, t, "LiteCore", "%lu %lu %d %d", param, param2, 0, (t % 5));
#else
        if (__builtin_available(macOS 10.12, iOS 10, tvOS 10, *))
            kdebug_signpost(t, param, param2, 0, (t % 5));
#endif
    }

    void Signpost::begin(Type t, uintptr_t param, uintptr_t param2) {
#if TARGET_OS_MACCATALYST || __MAC_OS_X_VERSION_MAX_ALLOWED >= 101500 || __IPHONE_OS_VERSION_MIN_REQUIRED >= 130000
        if (__builtin_available(iOS 12.0, macOS 10.14, *))
            os_signpost_interval_begin(LiteCore, t, "LiteCore", "%lu %lu %d %d", param, param2, 0, (t % 5));
#else
        if (__builtin_available(macOS 10.12, iOS 10, tvOS 10, *))
            kdebug_signpost_start(t, param, param2, 0, (t % 5));
#endif
    }

    void Signpost::end(Type t, uintptr_t param, uintptr_t param2) {
#if TARGET_OS_MACCATALYST || __MAC_OS_X_VERSION_MAX_ALLOWED >= 101500 || __IPHONE_OS_VERSION_MIN_REQUIRED >= 130000
        if (__builtin_available(iOS 12.0, macOS 10.14, *))
            os_signpost_interval_end(LiteCore, t, "LiteCore", "%lu %lu %d %d", param, param2, 0, (t % 5));
#else
        if (__builtin_available(macOS 10.12, iOS 10, tvOS 10, *))
            kdebug_signpost_end(t, param, param2, 0, (t % 5));
#endif
    }
#endif

}
