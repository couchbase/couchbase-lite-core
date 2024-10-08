#pragma once

#include "fleece/slice.hh"
#include <chrono>
#include <optional>
#include <slice_stream.hh>

namespace fleece {
    class DateFormat {
      public:
        static std::optional<DateFormat> parse(slice formatString);

        /** Formats a timestamp (milliseconds since 1/1/1970) as an ISO-8601 date-time.
        @param buf  The location to write the formatted C string. At least
                    kFormattedISO8601DateMaxSize bytes must be available.
        @param timestamp  The timestamp (milliseconds since 1/1/1970).
        @param asUTC  True to format as UTC, false to use the local time-zone.
        @param fmt The model to use for formatting (i.e. which portions to include).
                      If null, then the full ISO-8601 format is used
        @return  The formatted string (points to `buf`). */
        static slice format(char buf[], int64_t timestamp, bool asUTC, std::optional<DateFormat> fmt);

        /** Formats a timestamp (milliseconds since 1/1/1970) as an ISO-8601 date-time.
        @param buf  The location to write the formatted C string. At least
                    kFormattedISO8601DateMaxSize bytes must be available.
        @param timestamp  The timestamp (milliseconds since 1/1/1970).
        @param tzoffset   The timezone offset from UTC in minutes
        @param fmt The model to use for formatting (i.e. which portions to include).
                      If null, then the full ISO-8601 format is used
        @return  The formatted string (points to `buf`). */
        static slice format(char buf[], int64_t timestamp, std::chrono::minutes tzoffset,
                            std::optional<DateFormat> fmt);

        const static DateFormat kISO8601;

        enum class Timezone : uint8_t { NoColon, Colon };
        enum class Separator : char { Space = ' ', T = 'T' };

        struct YMD {
            enum class Separator : char { Hyphen = '-', Slash = '/' };

            explicit YMD(const Separator separator_) : separator(separator_) {}

            bool operator==(const YMD& other) const;

            const static YMD kISO8601;

            Separator separator;
        };

        struct HMS {
            enum class Separator : char { Colon = ':' };

            HMS(const bool millis_, const Separator separator_) : millis(millis_), separator(separator_) {}

            bool operator==(const HMS& other) const;

            const static HMS kISO8601;

            bool      millis;
            Separator separator;
        };

        // 1111-11-11T11:11:11(Z)
        DateFormat(YMD ymd_, Separator separator_, HMS hms_, const std::optional<Timezone> tz_ = {})
            : ymd{ymd_}, separator{separator_}, hms{hms_}, tz{tz_} {}

        // 11-11-11
        explicit DateFormat(YMD ymd_) : ymd{ymd_} {}

        // 11:11:11(Z)
        explicit DateFormat(HMS hms_, const std::optional<Timezone> tz_ = {}) : hms{hms_}, tz{tz_} {}

        bool operator==(const DateFormat& other) const;

        bool operator!=(const DateFormat& other) const { return !(*this == other); }

        explicit operator std::string() const;

      private:
        // %Y-%M-%DT%H:%M:%S
        static std::optional<DateFormat> parseTokenFormat(slice_istream formatStream);

        // 1111-11-11T11:11:11
        static std::optional<DateFormat> parseDateFormat(slice formatString);

        static std::optional<std::pair<Timezone, slice>> parseTimezone(slice formatString);

        static std::optional<std::pair<HMS, slice>> parseHMS(slice formatString);

        static std::optional<YMD> parseYMD(slice formatString);

        std::optional<YMD>       ymd;
        std::optional<Separator> separator;
        std::optional<HMS>       hms;
        std::optional<Timezone>  tz;
    };

    std::ostream& operator<<(std::ostream& os, DateFormat const& df);

    std::ostream& operator<<(std::ostream& os, std::optional<DateFormat> const& odf);

}  // namespace fleece
