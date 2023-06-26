//
//  Defer.hh
//  LiteCore
//
//  Created by Jens Alfke on 7/29/19.
//  Copyright 2019-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

//  Adapted from original ScopeGuard11 by Andrei Alexandrescu:
//  <https://gist.github.com/KindDragon/4650442>

#pragma once
#include <utility>

namespace litecore {

    template <class Fun>
    class ScopeGuard {
        Fun  f_;
        bool active_;

      public:
        explicit ScopeGuard(Fun f) : f_(std::move(f)), active_(true) {}

        ~ScopeGuard() {
            if ( active_ ) f_();
        }

        void dismiss() { active_ = false; }

        ScopeGuard()                             = delete;
        ScopeGuard(const ScopeGuard&)            = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;

        ScopeGuard(ScopeGuard&& rhs) noexcept : f_(std::move(rhs.f_)), active_(rhs.active_) { rhs.dismiss(); }
    };

    namespace detail {
        enum class ScopeGuardOnExit {};

        template <typename Fun>
        ScopeGuard<Fun> operator+(ScopeGuardOnExit, Fun&& fn) {
            return ScopeGuard<Fun>(std::forward<Fun>(fn));
        }
    }  // namespace detail

// I prefer the name DEFER to SCOPE_EXIT --jens
#define DEFER auto ANONYMOUS_VARIABLE(DEFERRED) = detail::ScopeGuardOnExit() + [&]()

#define CONCATENATE_IMPL(s1, s2) s1##s2
#define CONCATENATE(s1, s2)      CONCATENATE_IMPL(s1, s2)
#ifdef __COUNTER__
#    define ANONYMOUS_VARIABLE(str) CONCATENATE(str, __COUNTER__)
#else
#    define ANONYMOUS_VARIABLE(str) CONCATENATE(str, __LINE__)
#endif

}  // namespace litecore
