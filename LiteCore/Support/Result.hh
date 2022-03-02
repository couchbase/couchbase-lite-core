//
// Result.hh
//
// Copyright © 2022 Couchbase. All rights reserved.
//

#pragma once
#include <optional>
#include <variant>
#include <betterassert.hh>

struct C4Error;

namespace litecore {
    struct error;

    /// Represents the return value of a function that can fail.
    /// It contains either a value, or an error.
    /// The error type defaults to `liteore::error`.
    template <typename VAL, typename ERR =C4Error>
    class Result {
    public:
        /// A `Result` can be constructed from a value or an error.
        Result(const VAL &val) noexcept         :_result(val) { }
        Result(const ERR &err) noexcept         :_result(err) { }
        Result(VAL &&val) noexcept              :_result(std::move(val)) { }
        Result(ERR &&err) noexcept              :_result(std::move(err)) { }

        bool ok() const noexcept                {return _result.index() == 0;}
        bool isError() const noexcept           {return _result.index() != 0;}

        /// Returns the value. You must test first, as this will fail if there is an error!
        VAL& get() &                            {return *std::get_if<0>(&_result);}
        VAL&& get() &&                          {return std::move(*std::get_if<0>(&_result));}

        /// Returns the error. Throws an exception if there is none.
        const ERR& error() const noexcept       {return *std::get_if<1>(&_result);}

        /// Returns a pointer to the error, or nullptr if there is none.
        const ERR* errorPtr() const noexcept    {return std::get_if<1>(&_result);}

    private:
        std::variant<VAL,ERR> _result;
    };


    // Specialization of `Result` when there is no value.
    // Assumes `ERR` has a default no-error value and can be tested as a bool.
    template<typename ERR>
    class Result<void, ERR> {
    public:
        Result() noexcept                       :_error{} { }
        Result(const ERR &err) noexcept         :_error(err) { }
        Result(ERR &&err) noexcept              :_error(std::move(err)) { }

        bool ok() const noexcept                {return !_error;}
        bool isError() const noexcept           {return !!_error;}
        void get() const                        {precondition(!_error);}
        const ERR& error() const noexcept       {precondition(_error); return _error;}
        const ERR* errorPtr() const noexcept    {return _error ? &_error : nullptr;}

    private:
        ERR _error;
    };


    /// A `Result` whose error type is `C4Error`.
    template <typename VAL> using C4Result = Result<VAL, C4Error>;

}
