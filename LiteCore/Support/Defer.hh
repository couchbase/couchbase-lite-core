//
//  Defer.hh
//  LiteCore
//
//  Created by Jens Alfke on 7/29/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

//  Adapted from original ScopeGuard11 by Andrei Alexandrescu:
//  <https://gist.github.com/KindDragon/4650442>

#pragma once
#include <utility>

namespace litecore {

template <class Fun>
class ScopeGuard {
    Fun f_;
    bool active_;
public:
    ScopeGuard(Fun f)
    : f_(std::move(f))
    , active_(true) {
    }
    ~ScopeGuard() { if (active_) f_(); }
    void dismiss() { active_ = false; }
    ScopeGuard() = delete;
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&& rhs) noexcept
    : f_(std::move(rhs.f_))
    , active_(rhs.active_) {
        rhs.dismiss();
    }
};

namespace detail {
    enum class ScopeGuardOnExit {};
    template <typename Fun>
    ScopeGuard<Fun> operator+(ScopeGuardOnExit, Fun&& fn) {
            return ScopeGuard<Fun>(std::forward<Fun>(fn));
    }
}

// I prefer the name DEFER to SCOPE_EXIT --jens
#define DEFER \
    auto ANONYMOUS_VARIABLE(DEFERRED) \
        = detail::ScopeGuardOnExit() + [&]()

#define CONCATENATE_IMPL(s1, s2) s1##s2
#define CONCATENATE(s1, s2) CONCATENATE_IMPL(s1, s2)
#ifdef __COUNTER__
    #define ANONYMOUS_VARIABLE(str) \
        CONCATENATE(str, __COUNTER__)
#else
    #define ANONYMOUS_VARIABLE(str) \
        CONCATENATE(str, __LINE__)
#endif

}
