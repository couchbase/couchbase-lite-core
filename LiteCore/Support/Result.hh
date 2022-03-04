//
// Result.hh
//
// Copyright © 2022 Couchbase. All rights reserved.
//

#pragma once
#include "Defer.hh"             // for CONCATENATE()
#include "function_ref.hh"
#include <optional>
#include <variant>
#include <betterassert.hh>

struct C4Error;

namespace litecore {
    struct error;
    template <typename T, typename ERR =C4Error> class Result;

    namespace {
        // Magic template gunk. `unwrap_Result<T>` removes a layer of `Result<...>` from a type:
        // - `unwrap_Result<string>` is `string`.
        // - `unwrap_Result<Result<string>> is `string`.
        template <typename T> T _unwrap_Result(T*);
        template <typename T> T _unwrap_Result(Result<T>*);
        template <typename T> using unwrap_Result = decltype(_unwrap_Result((T*)nullptr));
    }


    /// Represents the return value of a function that can fail.
    /// It contains either a value of type T, or an error of type ERR (defaulting to C4Error).
    template <typename T, typename ERR>
    class Result {
    public:
        /// Constructs a successful Result from a value.
        Result(const T &val) noexcept         :_result(val) { }
        Result(T &&val) noexcept              :_result(std::move(val)) { }

        /// Constructs a failure Result from an error.
        /// The error must not be the empty/null/0 value.
        Result(const ERR &err) noexcept         :_result(err) {precondition(err);}
        Result(ERR &&err) noexcept              :_result(std::move(err)) {precondition(err);}

        /// True if successful.
        bool ok() const noexcept                {return _result.index() == 0;}

        /// True if not successful.
        bool isError() const noexcept           {return _result.index() != 0;}

        /// Returns the value. You must test first, as this will fail if there is an error!
        T& value() &                            {return *std::get_if<0>(&_result);}
        T value() &&                            {return std::move(*std::get_if<0>(&_result));}

        /// Returns the error, or the default empty/null/0 error if none.
        ERR error() const noexcept              {auto e = errorPtr(); return e ? *e : ERR{};}

        /// Returns a pointer to the error, or `nullptr` if there is none.
        const ERR* errorPtr() const noexcept    {return std::get_if<1>(&_result);}

        /// Transforms a `Result<T>` to a `Result<U>` by passing the value through a function.
        /// - If I have a value, I pass it to `fn` and return its result.
        ///   * If `fn` throws an exception, it's caught and returned (thanks to `TryResult()`.)
        /// - If I have an error, `fn` is _not_ called, and I return my error.
        /// @param fn  A function/lambda that takes a `T&&` and returns `U` or `Result<U>`.
        /// @return  The result of `fn`, or else my current error, as a `Result<U>`.
        template <typename LAMBDA,
                  typename RV = std::invoke_result_t<LAMBDA,T&&>,
                  typename U = unwrap_Result<RV>>
        [[nodiscard]]
        Result<U,ERR> then(LAMBDA fn) && noexcept {
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

    private:
        template <typename U>
        Result<U,ERR> _then(fleece::function_ref<U(T&&)> const& fn) noexcept;
        template <typename U>
        Result<U,ERR> _then(fleece::function_ref<Result<U,ERR>(T&&)> const& fn) noexcept;

        std::variant<T,ERR> _result;
    };


    // Specialization of `Result` when there is no value; just represents success or an error.
    // - The `success` constructor takes no arguments. Or you can construct with `kC4NoError`.
    // - There is no `value` method.
    // - The `then` callback takes no arguments.
    template<typename ERR>
    class Result<void, ERR> {
    public:
        Result() noexcept                       :_error{} { }
        Result(const ERR &err) noexcept         :_error(err) { }
        Result(ERR &&err) noexcept              :_error(std::move(err)) { }

        bool ok() const noexcept                {return !_error;}
        bool isError() const noexcept           {return !!_error;}
        const ERR& error() const noexcept       {return _error;}
        const ERR* errorPtr() const noexcept    {return _error ? &_error : nullptr;}

        template <typename LAMBDA,
                  typename RV = std::invoke_result_t<LAMBDA>,
                  typename U = unwrap_Result<RV>>
        [[nodiscard]]
        Result<U,ERR> then(LAMBDA fn) && noexcept {
            return _then<U>(fleece::function_ref<RV()>(std::forward<LAMBDA>(fn)));
        }

        template <typename LAMBDA>
        void onError(LAMBDA fn) {
            if (isError())
                fn(error());
        }

    private:
        template <typename U>
        Result<U,ERR> _then(fleece::function_ref<U()> const& fn) noexcept;
        template <typename U>
        Result<U,ERR> _then(fleece::function_ref<Result<U,ERR>()> const& fn) noexcept;

        ERR _error;
    };


    /// Runs a function returning `T` (or `Result<T>`) in a try/catch block,
    /// catching any exception and returning it as an error. Returns `Result<T>`.
    template <typename T>
    [[nodiscard]]
    Result<T> TryResult(fleece::function_ref<T()> fn) noexcept {
        try {
            return fn();
        } catch (std::exception &x) {
            return C4Error::fromException(x);
        }
    }


    /// Runs a function returning `Result<T>` in a try/catch block, catching any exception and
    /// returning it as an error. Returns `Result<T>`.
    template <typename T>
    [[nodiscard]]
    Result<T> TryResult(fleece::function_ref<Result<T>()> fn) noexcept {
        try {
            return fn();
        } catch (std::exception &x) {
            return C4Error::fromException(x);
        }
    }


    template <>
    [[nodiscard]]
    inline Result<void> TryResult(fleece::function_ref<void()> fn) noexcept {
        try {
            fn();
            return {};
        } catch (std::exception &x) {
            return C4Error::fromException(x);
        }
    }


    // (this helps the compiler deduce T when TryResult() is called with a lambda)
    template <typename LAMBDA,
              typename RV = std::invoke_result_t<LAMBDA>,
              typename T = unwrap_Result<RV>>
    [[nodiscard]]
    inline Result<T> TryResult(LAMBDA fn) noexcept {
        return TryResult<T>(fleece::function_ref<RV()>(std::move(fn)));
    }

    
    /// An approximation of Swift's `try` syntax for clean error propagation without exceptions.
    /// First `EXPR` is evaluated.
    /// - If the result is ok, the value is assigned to `VAR`, which may be an existing variable
    ///   name (`foo`) or a declaration (`int foo`).
    /// - If the result is an error, that error is returned from the current function, which should
    ///   have a return type of `Result<>` or `C4Error`.
    #define TRY_RESULT(VAR, EXPR)   \
        auto CONCATENATE(rslt, __LINE__) = (EXPR); \
        if (CONCATENATE(rslt, __LINE__).isError()) \
            return CONCATENATE(rslt, __LINE__).error(); \
        VAR = std::move(CONCATENATE(rslt, __LINE__)).value();
    // (`CONCATENATE(rslt, __LINE__)` is just a clumsy way to create a unique variable name.)


    //---- Method implementations


    template<typename T, typename ERR>
    template <typename U>
    [[nodiscard]]
    Result<U,ERR> Result<T,ERR>::_then(fleece::function_ref<U(T&&)> const& fn) noexcept {
        if (ok())
            return TryResult([&]{return fn(std::move(value()));});
        else
            return error();
    }

    template<typename T, typename ERR>
    template <typename U>
    [[nodiscard]]
    Result<U,ERR> Result<T,ERR>::_then(fleece::function_ref<Result<U,ERR>(T&&)> const& fn) noexcept {
        if (ok())
            return TryResult([&]{return fn(std::move(value()));});
        else
            return error();
    }


    template<typename ERR>
    template <typename U>
    [[nodiscard]]
    Result<U,ERR> Result<void,ERR>::_then(fleece::function_ref<U()> const& fn) noexcept {
        if (ok())
            return TryResult(fn);
        else
            return error();
    }

    template<typename ERR>
    template <typename U>
    [[nodiscard]]
    Result<U,ERR> Result<void,ERR>::_then(fleece::function_ref<Result<U,ERR>()> const& fn) noexcept {
        if (ok())
            return TryResult(fn);
        else
            return error();
    }

}
