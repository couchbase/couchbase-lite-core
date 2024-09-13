//
// c4EnumUtil.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

// Safe to disable this, these macros expand fine
// NOLINTBEGIN(bugprone-macro-parentheses)

#pragma once
#include <type_traits>

/// Declares `++` and `--` functions for an `enum class`.
#define DEFINE_ENUM_INC_DEC(E)                                                                                         \
    inline E operator++(E& e) {                                                                                        \
        ++(std::underlying_type_t<E>&)e;                                                                               \
        return e;                                                                                                      \
    }                                                                                                                  \
    inline E operator--(E& e) {                                                                                        \
        --(std::underlying_type_t<E>&)e;                                                                               \
        return e;                                                                                                      \
    }                                                                                                                  \
    inline E operator++(E& e, int) {                                                                                   \
        auto oldS = e;                                                                                                 \
        ++e;                                                                                                           \
        return oldS;                                                                                                   \
    }                                                                                                                  \
    inline E operator--(E& e, int) {                                                                                   \
        auto oldS = e;                                                                                                 \
        --e;                                                                                                           \
        return oldS;                                                                                                   \
    }


/// Declares `+`, `-`, `+=` and `-=` operators for an `enum class`,
/// which allow its underlying integer type to be added to / subtracted from it,
/// and allows subtracting two enum values producing the underlying integer type.
#define DEFINE_ENUM_ADD_SUB_INT(E)                                                                                     \
    inline E& operator+=(E& e, std::underlying_type_t<E> i) { return e = E(std::underlying_type_t<E>(e) + i); }        \
    inline E& operator-=(E& e, std::underlying_type_t<E> i) { return e = E(std::underlying_type_t<E>(e) - i); }        \
    inline E  operator+(E e, std::underlying_type_t<E> i) { return e += i; }                                           \
    inline E  operator-(E e, std::underlying_type_t<E> i) { return e -= i; }                                           \
    inline std::underlying_type_t<E> operator-(E e1, E e2) {                                                           \
        return std::underlying_type_t<E>(e1) - std::underlying_type_t<E>(e2);                                          \
    }

// NOLINTEND(bugprone-macro-parentheses)
