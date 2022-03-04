//
// Result.hh
//
// Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Error.h"
#include "Defer.hh"             // for CONCATENATE()
#include "function_ref.hh"
#include <type_traits>
#include <variant>
#include <betterassert.hh>

namespace litecore {
    template <typename T> class Result;

    // !!! Documentation is at docs/Result.md !!!

    namespace {
        // Magic template gunk. `unwrap_Result<T>` removes a layer of `Result<...>` from type T
        template <typename T> T _unwrap_Result(T*);
        template <typename T> T _unwrap_Result(Result<T>*);
        template <typename T> using unwrap_Result = decltype(_unwrap_Result((T*)nullptr));
    }


    /// Represents the return value of a function that can fail.
    /// It contains either a value of type T, or a C4Error.
    template <typename T>
    class Result {
    public:
        /// Constructs a successful Result from a value.
        Result(const T &val) noexcept           :_result(val) { }
        Result(T &&val) noexcept                :_result(std::move(val)) { }

        /// Constructs a failure Result from an error.
        /// The error must not be the empty/null/0 value.
        Result(const C4Error &err) noexcept     :_result(err) {precondition(err);}
        Result(C4Error &&err) noexcept          :_result(std::move(err)) {precondition(err);}

        /// True if successful.
        bool ok() const noexcept                {return _result.index() == 0;}

        /// True if not successful.
        bool isError() const noexcept           {return _result.index() != 0;}

        /// Returns the value. Or if there's an error, throws it as an exception(!)
        T& value() & {
            if (auto e = errorPtr(); _usuallyFalse(e != nullptr)) e->raise();
            return *std::get_if<0>(&_result);
        }

        T value() && {
            if (auto e = errorPtr(); _usuallyFalse(e != nullptr)) e->raise();
            return std::move(*std::get_if<0>(&_result));
        }

        /// Returns the error, or an empty C4Error with code==0 if none.
        C4Error error() const noexcept          {auto e = errorPtr(); return e ? *e : C4Error{};}

        /// Returns a pointer to the error, or `nullptr` if there is none.
        const C4Error* errorPtr() const noexcept {return std::get_if<1>(&_result);}

        /// Transforms a `Result<T>` to a `Result<U>` by passing the value through a function.
        /// - If I have a value, I pass it to `fn` and return its result.
        ///   * If `fn` throws an exception, it's caught and returned (thanks to `CatchResult()`.)
        /// - If I have an error, `fn` is _not_ called, and I return my error.
        /// @param fn  A function/lambda that takes a `T&&` and returns `U` or `Result<U>`.
        /// @return  The result of `fn`, or else my current error, as a `Result<U>`.
        template <typename LAMBDA,
                  typename RV = std::invoke_result_t<LAMBDA,T&&>,
                  typename U = unwrap_Result<RV>>
        [[nodiscard]]
        Result<U> then(LAMBDA fn) && noexcept {
            return _then<U>(fleece::function_ref<RV(T&&)>(std::forward<LAMBDA>(fn)));
        }

        /// Calls `fn` with the error, if there is one, else does nothing.
        /// @param fn  A function/lambda that takes a `C4Error` and returns `void`.
        /// @return  Always returns itself, `*this`.
        template <typename LAMBDA>
        [[nodiscard]]
        Result& onError(LAMBDA fn) {
            if (isError())
                fn(error());
            return *this;
        }

        // `_value` is faster than `value`, but you MUST have preflighted or it'll deref NULL.
        T& _value() & noexcept                  {return *std::get_if<0>(&_result);}
        T _value() && noexcept                  {return std::move(*std::get_if<0>(&_result));}

    private:
        template <typename U>
        Result<U> _then(fleece::function_ref<U(T&&)> const& fn) noexcept;
        template <typename U>
        Result<U> _then(fleece::function_ref<Result<U>(T&&)> const& fn) noexcept;

        std::variant<T,C4Error> _result;
    };


    // Specialization of `Result` when there is no value; it just represents success or an error.
    // - The `success` constructor takes no arguments. Or you can construct with `kC4NoError`.
    // - There is no `value` method.
    // - The `then` callback takes no arguments.
    template<>
    class Result<void> {
    public:
        Result() noexcept                           :_error{} { }
        Result(const C4Error &err) noexcept         :_error(err) { }
        Result(C4Error &&err) noexcept              :_error(std::move(err)) { }

        bool ok() const noexcept                    {return !_error;}
        bool isError() const noexcept               {return !!_error;}
        const C4Error& error() const noexcept       {return _error;}
        const C4Error* errorPtr() const noexcept    {return _error ? &_error : nullptr;}

        template <typename LAMBDA,
                  typename RV = std::invoke_result_t<LAMBDA>,
                  typename U = unwrap_Result<RV>>
        [[nodiscard]]
        Result<U> then(LAMBDA fn) && noexcept {
            return _then<U>(fleece::function_ref<RV()>(std::forward<LAMBDA>(fn)));
        }

        template <typename LAMBDA>
        void onError(LAMBDA fn) {
            if (isError())
                fn(error());
        }

    private:
        template <typename U>
        Result<U> _then(fleece::function_ref<U()> const& fn) noexcept;
        template <typename U>
        Result<U> _then(fleece::function_ref<Result<U>()> const& fn) noexcept;

        C4Error _error;
    };


    /// Runs a function returning `T` in a try/catch block,
    /// catching any exception and returning it as an error. Returns `Result<T>`.
    template <typename T>
    [[nodiscard]]
    Result<T> CatchResult(fleece::function_ref<T()> fn) noexcept {
        try {
            return fn();
        } catch (std::exception &x) {
            return C4Error::fromException(x);
        }
    }


    /// Runs a function returning `Result<T>` in a try/catch block,
    /// catching any exception and returning it as an error. Returns `Result<T>`.
    template <typename T>
    [[nodiscard]]
    Result<T> CatchResult(fleece::function_ref<Result<T>()> fn) noexcept {
        try {
            return fn();
        } catch (std::exception &x) {
            return C4Error::fromException(x);
        }
    }


    // (specialization needed for T=void)
    template <>
    [[nodiscard]]
    inline Result<void> CatchResult(fleece::function_ref<void()> fn) noexcept {
        try {
            fn();
            return {};
        } catch (std::exception &x) {
            return C4Error::fromException(x);
        }
    }


    // (this helps the compiler deduce T when CatchResult() is called with a lambda)
    template <typename LAMBDA,
              typename RV = std::invoke_result_t<LAMBDA>, // return value
              typename T = unwrap_Result<RV>>             // RV with `Result<...>` stripped off
    [[nodiscard]]
    inline Result<T> CatchResult(LAMBDA fn) noexcept {
        return CatchResult<T>(fleece::function_ref<RV()>(std::move(fn)));
    }

    
    /// An approximation of Swift's `try` syntax for clean error propagation without exceptions.
    /// First `EXPR` is evaluated.
    /// - If the result is ok, the value is assigned to `VAR`, which may be an existing variable
    ///   name (`foo`) or a declaration (`int foo`).
    /// - If the result is an error, that error is returned from the current function, which should
    ///   have a return type of `Result<>` or `C4Error`.
    #define TRY(VAR, EXPR)   \
        auto CONCATENATE(rslt, __LINE__) = (EXPR); \
        if (CONCATENATE(rslt, __LINE__).isError()) \
            return CONCATENATE(rslt, __LINE__).error(); \
        VAR = std::move(CONCATENATE(rslt, __LINE__))._value();
    // (`CONCATENATE(rslt, __LINE__)` is just a clumsy way to create a unique variable name.)


    //---- Method implementations


    template<typename T>
    template <typename U>
    [[nodiscard]]
    Result<U> Result<T>::_then(fleece::function_ref<U(T&&)> const& fn) noexcept {
        if (ok())
            return CatchResult([&]{return fn(std::move(_value()));});
        else
            return error();
    }

    template<typename T>
    template <typename U>
    [[nodiscard]]
    Result<U> Result<T>::_then(fleece::function_ref<Result<U>(T&&)> const& fn) noexcept {
        if (ok())
            return CatchResult([&]{return fn(std::move(_value()));});
        else
            return error();
    }


    template <typename U>
    [[nodiscard]]
    Result<U> Result<void>::_then(fleece::function_ref<U()> const& fn) noexcept {
        if (ok())
            return CatchResult(fn);
        else
            return error();
    }

    template <typename U>
    [[nodiscard]]
    Result<U> Result<void>::_then(fleece::function_ref<Result<U>()> const& fn) noexcept {
        if (ok())
            return CatchResult(fn);
        else
            return error();
    }

}
