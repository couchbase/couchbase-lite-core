//
// Created by Jens Alfke on 11/11/24.
//

#pragma once
#include "StringUtil.hh"
#include <algorithm>
#include <optional>

namespace litecore::REST {

    /** A parsed MIME type. Supports parameters but doesn't parse them.
        The type and subtype are syntax-checked according to RFC 2045 sec 5.1,
        but the parameter portion isn't, except to verify there are no control characters. */
    class MIMEType {
      public:
        /// Returns a MIMEType parsed from a string, or `nullopt` if invalid.
        static std::optional<MIMEType> parse(std::string_view str) {
            auto slashAndEnd = _parse(str);
            if ( slashAndEnd.second == 0 ) return std::nullopt;
            return MIMEType(str, slashAndEnd);
        }

        /// Constructor from string; will throw if invalid.
        /// @throws std::invalid_argument if invalid
        explicit MIMEType(std::string_view type) : MIMEType(type, _parse(type)) {}

        /// Constructor from a primary type and subtype. The input strings must be valid.
        MIMEType(std::string_view type, std::string_view subtype) : _str(std::string(type) + '/') {
            _str += subtype;
            _slash     = type.size();
            _endOfType = _str.size();
            toLowercase();
        }

        operator std::string_view() { return _str; }

        /// The primary type, like 'text' or 'image'. Always lowercase.
        std::string_view mediaType() const { return std::string_view(_str).substr(_slash); }

        /// The subtype. Always lowercase. */
        std::string_view subType() const {
            return std::string_view(_str).substr(_slash + 1, _endOfType - (_slash + 1));
        }

        /// The type and subtype, i.e. the MIME type minus any parameters.
        std::string_view fullType() const { return std::string_view(_str).substr(0, _endOfType); }

        /// The optional parameters, i.e. everything after the first ';'.
        std::string_view parameters() const {
            size_t start = _endOfType;
            if ( start < _str.size() ) ++start;  // skip the ';'
            return std::string_view(_str).substr(start);
        }

        friend bool operator==(const MIMEType& a, const MIMEType& b) { return a.fullType() == b.fullType(); }

        friend bool operator==(const MIMEType& a, std::string_view str) { return equalTokens(a.fullType(), str); }

        /// Compares two MIME types; either may have a "*" wildcard as media type or subtype.
        bool matches(const MIMEType& other) const { return matches(other.mediaType(), other.subType()); }

        bool matches(std::string_view otherMediaType, std::string_view otherSubtype) const {
            return matchTokens(mediaType(), otherMediaType) && matchTokens(subType(), otherSubtype);
        }

      private:
        // Parse type & subtype according to https://www.rfc-editor.org/rfc/rfc2045#section-5.1
        static std::pair<size_t, size_t> _parse(std::string_view str) {
            // First reject any control characters or non-ASCII characters:
            if ( std::ranges::count_if(str, [](char ch) { return ch < ' ' || ch >= 127; }) ) return {0, 0};

            auto i = std::find_if(str.begin(), str.end(), nonTokenChar);
            if ( i == str.end() || *i != '/' ) return {0, 0};
            size_t slash = i - str.begin();

            i = std::find_if(i + 1, str.end(), nonTokenChar);
            if ( i != str.end() && *i != ';' ) return {0, 0};
            size_t endOfType = i - str.begin();
            return {slash, endOfType};
        }

        MIMEType(std::string_view str, std::pair<size_t, size_t> slashAndEnd)
            : _str(str), _slash(slashAndEnd.first), _endOfType(slashAndEnd.second) {
            if ( _endOfType == 0 ) throw std::invalid_argument("invalid MIME type " + _str);
            toLowercase();
        }

        static bool nonTokenChar(char c) {
            static constexpr const char* kSpecials = " ()<>@,;:\"\\/[]?=";
            return strchr(kSpecials, c) != nullptr;
        }

        static bool equalTokens(std::string_view t1, std::string_view t2) {
            return slice(t1).caseEquivalentCompare(t2) == 0;
        }

        static bool matchTokens(std::string_view t1, std::string_view t2) {
            return equalTokens(t1, t2) || t1 == "*" || t2 == "*";
        }

        // Lowercase the type and subtype, but not the parameters (which are case-sensitive)
        void toLowercase() {
            std::for_each(_str.begin(), _str.begin() + _endOfType, [](char& c) { c = char(tolower(uint8_t(c))); });
        }

        std::string _str;        // The entire MIME type string
        size_t      _slash;      // The byte index of the '/' separator
        size_t      _endOfType;  // The byte index of the end of the subtype
    };

}  // namespace litecore::REST
