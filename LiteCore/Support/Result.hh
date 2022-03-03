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
    /// The error type defaults to `C4Error`.
    template <typename VAL, typename ERR =C4Error>
    class Result {
    public:
        /// Constructs a successful Result from a value.
        Result(const VAL &val) noexcept         :_result(val) { }
        Result(VAL &&val) noexcept              :_result(std::move(val)) { }

        /// Constructs a failure Result from an error.
        /// The error must not be the empty/null/0 value.
        Result(const ERR &err) noexcept         :_result(err) {precondition(err);}
        Result(ERR &&err) noexcept              :_result(std::move(err)) {precondition(err);}

        /// True if successful.
        bool ok() const noexcept                {return _result.index() == 0;}

        /// True if not successful.
        bool isError() const noexcept           {return _result.index() != 0;}

        /// Returns the value. You must test first, as this will fail if there is an error!
        VAL& value() &                          {return *std::get_if<0>(&_result);}
        VAL&& value() &&                        {return std::move(*std::get_if<0>(&_result));}

        /// Returns the error, or the default empty/null/0 error if none.
        ERR error() const noexcept              {auto e = errorPtr(); return e ? *e : ERR{};}

        /// Returns a pointer to the error, or `nullptr` if there is none.
        const ERR* errorPtr() const noexcept    {return std::get_if<1>(&_result);}

    private:
        std::variant<VAL,ERR> _result;
    };


    // Specialization of `Result` when there is no value; just represents success or an error.
    // This just stores the ERR value, which is empty/null/0 in the success case.
    // Assumes `ERR` has a default no-error value and can be tested as a bool.
    template<typename ERR>
    class Result<void, ERR> {
    public:
        /// Constructs a successful Result.
        Result() noexcept                       :_error{} { }

        /// Constructs a failure Result from an error.
        /// The error may be the empty/null/0 value, in which case it means success.
        Result(const ERR &err) noexcept         :_error(err) { }
        Result(ERR &&err) noexcept              :_error(std::move(err)) { }

        bool ok() const noexcept                {return !_error;}
        bool isError() const noexcept           {return !!_error;}
        const ERR& error() const noexcept       {return _error;}
        const ERR* errorPtr() const noexcept    {return _error ? &_error : nullptr;}

    private:
        ERR _error;
    };


    /// A `Result` whose error type is `C4Error`.
    template <typename VAL> using C4Result = Result<VAL, C4Error>;

}
