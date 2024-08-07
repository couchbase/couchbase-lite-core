//
// WeakRef.hh
//
// Copyright © 2024 Couchbase. All rights reserved.
//

#pragma once
#include "c4Compat.h"
#include "Error.hh"  // for Assert()

C4_ASSUME_NONNULL_BEGIN

namespace litecore::qt {

    /** A class that a `checked_ptr<T>` can point to. It tracks the number of them pointing to it.
        Deleting a `checked_target` while there are still `checked_ptr`s pointing to it is illegal,
        and will trigger an assertion failure. */
    class checked_target {
      public:
        ~checked_target() {
            Assert(_refs == 0, "checked_target being deleted while it still has %d checked_ptrs pointing to it", _refs);
            // (unfortunately we can't say what subclass it is,
            // because that info's been lost by the time we're in the base class destructor.)
        }

      private:
        friend class checked_ptr_base;
        int _refs = 0;
    };

    // base class of `checked_ptr`. Factors out the non-template stuff.
    class checked_ptr_base {
      public:
        checked_ptr_base() = default;

        explicit checked_ptr_base(checked_target* C4NULLABLE r) noexcept { attach(r); }

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

        checked_ptr_base& operator=(checked_target* C4NULLABLE r) noexcept {
            if ( _usuallyTrue(r != _ref) ) {
                detach();
                attach(r);
            }
            return *this;
        }

      protected:
        inline void attach(checked_target* C4NULLABLE r) noexcept {
            _ref = r;
            if ( r ) ++r->_refs;
        }

        inline void detach() {
            if ( auto r = _ref ) {
                DebugAssert(r->_refs > 0);
                --r->_refs;
            }
        }

        checked_target* C4NULLABLE _ref = nullptr;
    };

    /** A (nullable) pointer to an instance of some subclass T of `checked_target`.
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

        T* C4NULLABLE get() const noexcept { return static_cast<T*>(_ref); }

        operator T* C4NULLABLE() const noexcept { return get(); }

        T* C4NULLABLE operator->() const noexcept { return get(); }
    };

}  // namespace litecore::qt

C4_ASSUME_NONNULL_END
