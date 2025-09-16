//
// SQLUtil.hh
//
// Copyright © 2022 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"
#include <iomanip>
#include <ostream>
#include <string_view>
#include <utility>

namespace litecore {

    /// True if the slice contains only ASCII alphanumerics and underscores (and is non-empty.)
    bool isAlphanumericOrUnderscore(slice str);


    /// True if the slice is a valid SQL identifier that doesn't require double-quotes,
    /// i.e. it isAlphanumericOrUnderscore and does not begin with a digit.
    bool isValidIdentifier(slice str);

    /// Wrapper object for a slice/string, which when written to an `ostream` puts the `QUOTE`
    /// character before & after the string, and prefixes any occurrences of `QUOTE` with `ESC`.
    ///
    /// This has the SQL-specific behavior that, when `QUOTE` and `ESC` are both `"`, it does
    /// nothing if the the string is a valid SQL identifier and so doesn't need it.
    ///
    /// You should use the `sqlString()` and `sqlIdentifier()` functions instead of this directly.
    template <char QUOTE, char ESC>
    struct quotedSlice {
        explicit quotedSlice(slice s) : _raw(std::move(s)) {}

        quotedSlice(const quotedSlice&) = delete;
        quotedSlice(quotedSlice&&)      = delete;

        slice const _raw;
    };

    template <char QUOTE, char ESC>
    std::ostream& operator<<(std::ostream& out, const quotedSlice<QUOTE, ESC>& str) {
        // SQL strings ('') are always quoted; identifiers ("") only when necessary.
        if ( QUOTE == '"' && ESC == '"' && isValidIdentifier(str._raw) ) {
            out.write((const char*)str._raw.buf, str._raw.size);
        } else {
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 9
            // (GCC 7.4 does not have std::quoted(string_view) for some reason, though GCC 9 does)
            out << std::quoted(std::string(str._raw), QUOTE, ESC);
#else
            out << std::quoted(std::string_view(str._raw), QUOTE, ESC);
#endif
        }
        return out;
    }

    /// Wrap around a string when writing to a stream, to single-quote it as a SQL string literal
    /// and escape any single-quotes it contains:
    /// `out << sqlString("I'm a string");` --> `'I''m a string'`
    inline auto sqlString(slice str) { return quotedSlice<'\'', '\''>(str); }

    /// Wrap around a SQL identifier when writing to a stream, to double-quote it if necessary:
    /// `out << sqlIdentifier("normal_identifier") --> `normal_identifier`
    /// `out << sqlIdentifier("weird/\"identifier\"");` --> `"weird/""identifier"""`
    inline auto sqlIdentifier(slice name) { return quotedSlice<'"', '"'>(name); }

}  // namespace litecore
