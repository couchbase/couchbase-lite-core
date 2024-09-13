//
//  Any.hh
//
// Copyright (c) 2019 Couchbase, Inc. All rights reserved.
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

// Adapted from Any.h in the ANTLR source code:
// <https://github.com/antlr/antlr4/blob/master/runtime/Cpp/runtime/src/support/Any.h>
// which is licensed as:
/* Copyright (c) 2012-2017 The ANTLR Project. All rights reserved.
 * Use of this file is governed by the BSD 3-clause license that
 * can be found in the LICENSE.txt file in the project root.
 */

// A standard C++ class loosely modeled after boost::Any.
// The API is *NOT* identical to C++17's std::any!

#pragma once
#include <algorithm>
#include <typeinfo>
#include <type_traits>
#include <utility>

#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4521)  // 'litecore::Any': multiple copy constructors specified
#endif

namespace litecore {

    template <class T>
    using StorageType = typename std::decay<T>::type;

    struct Any {
        [[nodiscard]] bool isNull() const { return _ptr == nullptr; }

        [[nodiscard]] bool isNotNull() const { return _ptr != nullptr; }

        Any() : _ptr(nullptr) {}

        Any(Any& that) : _ptr(that.clone()) {}

        Any(Any&& that) noexcept : _ptr(that._ptr) { that._ptr = nullptr; }

        Any(const Any& that) : _ptr(that.clone()) {}

        Any(const Any&& that) noexcept : _ptr(that.clone()) {}

        // Making the constructors in this class explicit breaks half the code
        // NOLINTBEGIN(google-explicit-constructor)

        // Only enable for types that aren't `Any` to avoid the compiler choosing this over copy or move
        template <typename U, typename = std::enable_if_t<!std::is_same_v<Any, std::decay_t<U>>>>
        Any(U&& value) : _ptr(new Derived<StorageType<U>>(std::forward<U>(value))) {}

        template <class U>
        [[nodiscard]] bool is() const {
            auto derived = getDerived<U>(false);

            return derived != nullptr;
        }

        template <class U>
        StorageType<U>& as() {
            auto derived = getDerived<U>(true);

            return derived->value;
        }

        template <class U>
        const StorageType<U>& as() const {
            auto derived = getDerived<U>(true);

            return derived->value;
        }

        template <class U>
        operator U() {
            return as<StorageType<U>>();
        }

        // NOLINTEND(google-explicit-constructor)

        template <class U>
        explicit operator U() const {
            return as<const StorageType<U>>();
        }

        template <class U, class LAMBDA>  //jpa: Added this
        bool with(LAMBDA lambda) const {
            auto derived = getDerived<U>(false);
            if ( !derived ) return false;
            lambda(derived->value);
            return true;
        }

        Any& operator=(const Any& a) {
            if ( this == &a ) return *this;

            auto old_ptr = _ptr;
            _ptr         = a.clone();

            delete old_ptr;

            return *this;
        }

        Any& operator=(Any&& a) noexcept {
            if ( _ptr == a._ptr ) return *this;

            std::swap(_ptr, a._ptr);

            return *this;
        }

        ~Any() { delete _ptr; }

        [[nodiscard]] bool equals(const Any& other) const { return _ptr == other._ptr; }

        bool operator==(const Any& other) const { return this->_ptr == other._ptr; }

      private:
        struct Base {
            virtual ~Base() = default;
            ;
            [[nodiscard]] virtual Base* clone() const = 0;
        };

        template <typename T>
        struct Derived : Base {
            template <typename U, typename = std::enable_if_t<!std::is_same_v<Derived, std::decay_t<U>>>>
            explicit Derived(U&& value_) : value(std::forward<U>(value_)) {}

            T value;

            //jpa: Changed `is_nothrow_copy_constructible` to `is_copy_constructible` since otherwise
            // the Any class doesn't work with common types like std::string.
            [[nodiscard]] Base* clone() const override {
                if constexpr ( std::is_copy_constructible_v<T> ) return new Derived<T>(value);
                else
                    return nullptr;
            }
        };

        [[nodiscard]] Base* clone() const {
            if ( _ptr ) return _ptr->clone();
            else
                return nullptr;
        }

        template <class U>
        Derived<StorageType<U>>* getDerived(bool checkCast) const {
            typedef StorageType<U> T;

            auto derived = dynamic_cast<Derived<T>*>(_ptr);

            if ( checkCast && !derived ) throw std::bad_cast();

            return derived;
        }

        Base* _ptr;
    };

    template <>
    inline Any::Any(std::nullptr_t&&) : _ptr(nullptr) {}


}  // namespace litecore

#ifdef _MSC_VER
#    pragma warning(pop)
#endif
