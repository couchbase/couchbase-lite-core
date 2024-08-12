//
// checked_ptr.hh
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Compat.h"
#include "Error.hh"  // for Assert()
#include "Logging.hh"
#include <iostream>  //TEMP

#if DEBUG
#    include "Backtrace.hh"
#    include <typeinfo>
#endif

C4_ASSUME_NONNULL_BEGIN

namespace litecore {

    /** A class that a `checked_ptr<T>` can point to. It tracks the number of them pointing to it.
        @warning  Deleting a `checked_target` while there are still `checked_ptr`s pointing to it is illegal
                  -- that's kind of the whole point of this API --
                  and will trigger an assertion failure.
        @warning  This class is not thread-safe. */
    class checked_target {
      public:
        virtual ~checked_target() {
            if ( _usuallyFalse(_checked_refs > 0 && !std::uncaught_exceptions()) ) {
#if DEBUG
                Backtrace().writeTo(std::cerr);  // TEMP debugging Linux crash
                string className = Unmangle(*_myType);
#else
                string className = "checked_target";
#endif
                Assert(false, "%s being deleted while it still has %d checked_ptrs pointing to it", className.c_str(),
                       _checked_refs);
            }
        }

      private:
        friend class checked_ptr_base;

        void pointer_added() const noexcept {
            ++_checked_refs;
#if DEBUG
            if ( !_myType ) _myType = &typeid(*this);
#endif
        }

        void pointer_removed() const noexcept {
            --_checked_refs;
            DebugAssert(_checked_refs >= 0);
        }

        mutable int _checked_refs = 0;
#if DEBUG
        mutable std::type_info const* C4NULLABLE _myType = nullptr;
#endif
    };

    // base class of `checked_ptr`. Factors out the non-template stuff.
    class checked_ptr_base {
      public:
        checked_ptr_base() = default;

        explicit checked_ptr_base(checked_target const* C4NULLABLE r) noexcept { attach(r); }

        checked_ptr_base(checked_ptr_base const& w) noexcept : checked_ptr_base(w._ref) {}

        checked_ptr_base(checked_ptr_base&& w) noexcept : _ref(w._ref) { w._ref = nullptr; }

        ~checked_ptr_base() noexcept { detach(); }

        checked_ptr_base& operator=(checked_ptr_base const& w) noexcept {
            *this = w._ref;
            return *this;
        }

        checked_ptr_base& operator=(checked_ptr_base&& w) noexcept {
            if ( _usuallyTrue(w._ref != _ref) ) {
                detach();
                _ref   = w._ref;
                w._ref = nullptr;
            }
            return *this;
        }

        checked_ptr_base& operator=(checked_target const* C4NULLABLE r) noexcept {
            if ( _usuallyTrue(r != _ref) ) {
                detach();
                attach(r);
            }
            return *this;
        }

      protected:
        inline void attach(checked_target const* C4NULLABLE r) noexcept {
            _ref = r;
            if ( r ) r->pointer_added();
        }

        inline void detach() {
            if ( _ref ) _ref->pointer_removed();
        }

        checked_target const* C4NULLABLE _ref = nullptr;
    };

    /** A (nullable) non-owning smart pointer to an instance of some subclass T of `checked_target`.
        Deleting the target while this points to it is an error and will be caught as an assertion failure. */
    template <class T>
    class checked_ptr : checked_ptr_base {
      public:
        checked_ptr() = default;

        explicit checked_ptr(T* C4NULLABLE t) noexcept : checked_ptr_base(static_cast<checked_target*>(t)) {}

        checked_ptr(checked_ptr const& w) noexcept : checked_ptr_base(w) {}

        checked_ptr(checked_ptr&& w) noexcept : checked_ptr_base(std::move(w)) {}

        checked_ptr& operator=(checked_ptr const& w) noexcept {
            checked_ptr_base::operator=(w);
            return *this;
        }

        checked_ptr& operator=(checked_ptr&& w) noexcept {
            checked_ptr_base::operator=(std::move(w));
            return *this;
        }

        checked_ptr& operator=(T* C4NULLABLE n) noexcept {
            checked_ptr_base::operator=(n);
            return *this;
        }

        T* C4NULLABLE get() const noexcept { return static_cast<T*>(const_cast<checked_target*>(_ref)); }

        operator T* C4NULLABLE() const noexcept { return get(); }

        T* C4NULLABLE operator->() const noexcept { return get(); }
    };

}  // namespace litecore

C4_ASSUME_NONNULL_END
