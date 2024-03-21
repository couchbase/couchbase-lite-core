#pragma once

#include "ParseDate.hh"
#include "date/date.h"
#include <chrono>
#include <cmath>
#include <ratio>
#include <cstdint>
#include "SQLiteFleeceUtil.hh"
#include "DateFormat.hh"

using namespace std::chrono;
using namespace date;
using namespace fleece;

namespace date {

    using quarters    = duration<int, detail::ratio_multiply<std::ratio<3>, months::period>>;
    using decades     = duration<int, detail::ratio_multiply<std::ratio<10>, years::period>>;
    using centuries   = duration<int, detail::ratio_multiply<std::ratio<100>, years::period>>;
    using millenniums = duration<int, detail::ratio_multiply<std::ratio<1000>, years::period>>;

    typedef struct {
        int64_t year;
        int64_t doy;
        int64_t hour;
        int64_t minute;
        int64_t second;
        int64_t millisecond;
    } DateDiff;

}  // namespace date

namespace litecore {
    // A number of functions in this file are borrowed directly from the Server N1QL code, with slight modifications.
    // The origials can be found here: https://github.com/couchbase/query/blob/master/expression/func_date.go.
    // This is important, because it means we get the same results as Server N1QL for our date manipulation functions.
    // I (Callum Birks) have tried rewriting parts of this file to use the date library rather than custom functions,
    // but it's not really possible while keeping the results identical to Server N1QL. I would advise the future reader
    // against attempting to rewrite this file.

    // Which quarter this date is in (Q1, Q2, ...)
    inline int64_t getQuarter(const DateTime& t) { return (t.M + 2) / 3; }

    // The number of leap years up until the given year
    inline int64_t leapYearsWithin(int64_t year) {
        if ( year > 0 ) {
            year--;
        } else {
            year++;
        }

        return (year / 4) - (year / 100) + (year / 400);
    }

    // The number of leap years between the given years
    inline int64_t leapYearsBetween(const int64_t start, const int64_t end) {
        return leapYearsWithin(start) - leapYearsWithin(end);
    }

    // The Day Of Year for the given time_point. This is the number of days since the start of the year.
    inline int64_t doy(const system_clock::time_point& t) {
        const auto daypoint = floor<days>(t);
        const auto ymd      = year_month_day{daypoint};
        const auto year     = ymd.year();
        const auto year_day = daypoint - sys_days{year / January / 0};
        return year_day.count();
    }

    inline system_clock::time_point to_time_point(DateTime& dt, bool no_tz = false) {
        const auto millis = ToMillis(dt, no_tz);
        return system_clock::time_point(milliseconds(millis));
    }

    inline bool parseDateArg(sqlite3_value* arg, int64_t* outTime) {
        const auto str = stringSliceArgument(arg);
        return str && kInvalidDate != (*outTime = ParseISO8601Date(str));
    }

    inline std::optional<DateFormat> parseDateFormat(sqlite3_value* arg) {
        if ( sqlite3_value_type(arg) != SQLITE_TEXT ) return {};
        const auto str = valueAsStringSlice(arg);
        if ( !str ) return {};

        return DateFormat::parse(str);
    }

    inline bool parseDateArgRaw(sqlite3_value* arg, DateTime* outTime) {
        if ( sqlite3_value_type(arg) != SQLITE_TEXT ) return false;
        const auto str = valueAsStringSlice(arg);
        if ( !str ) { return false; }

        *outTime = ParseISO8601DateRaw(str);
        return outTime->validYMD || outTime->validHMS;
    }

    inline void setResultDateString(sqlite3_context* ctx, const int64_t millis, const minutes tz_offset,
                                    const std::optional<DateFormat>& format) {
        char buf[kFormattedISO8601DateMaxSize];
        setResultTextFromSlice(ctx, DateFormat::format(buf, millis, tz_offset, format));
    }

    inline void setResultDateString(sqlite3_context* ctx, const int64_t millis, const bool asUTC,
                                    const std::optional<DateFormat>& format) {
        char buf[kFormattedISO8601DateMaxSize];
        setResultTextFromSlice(ctx, DateFormat::format(buf, millis, asUTC, format));
    }

    inline int64_t diffPart(const DateTime& t1, const DateTime& t2, const DateDiff& diff, const DateComponent part) {
        switch ( part ) {
            case kDateComponentMillisecond:
                {
                    const auto sec = diffPart(t1, t2, diff, kDateComponentSecond);
                    return sec * 1000 + diff.millisecond;
                }
            case kDateComponentSecond:
                {
                    const auto min = diffPart(t1, t2, diff, kDateComponentMinute);
                    return min * 60 + diff.second;
                }
            case kDateComponentMinute:
                {
                    const auto hour = diffPart(t1, t2, diff, kDateComponentHour);
                    return hour * 60 + diff.minute;
                }
            case kDateComponentHour:
                {
                    const auto days = diffPart(t1, t2, diff, kDateComponentDay);
                    return days * 24 + diff.hour;
                }
            case kDateComponentDay:
                {
                    auto days = diff.year * 365 + diff.doy;
                    if ( diff.year != 0 ) { days += leapYearsBetween(t1.Y, t2.Y); }

                    return days;
                }
            case kDateComponentWeek:
                {
                    const auto days = diffPart(t1, t2, diff, kDateComponentDay);
                    return days / 7;
                }
            case kDateComponentMonth:
                return abs((t1.Y * 12 + t1.M) - (t2.Y * 12 + t2.M));
            case kDateComponentQuarter:
                return abs(t1.Y * 4 + getQuarter(t1)) - (t2.Y * 4 + getQuarter(t2));
            case kDateComponentYear:
                return diff.year;
            case kDateComponentDecade:
                return diff.year / 10;
            case kDateComponentCentury:
                return diff.year / 100;
            case kDateComponentMillennium:
                return diff.year / 1000;
            default:
                return -1;
        }
    }

    static double frac(const double v) {
        double temp;
        return std::modf(v, &temp);
    }

    // The difference in the given date component between the two given dates.
    // An important distinction: "difference between the years of the two dates" rather than "difference in years
    // between the two dates".
    // i.e. diff(2018-01-01, 2017-12-31, "years") == 1
    inline void doDateDiff(sqlite3_context* ctx, DateTime left, DateTime right, const slice& part) {
        DateComponent date_component;
        if ( !part || (date_component = ParseDateComponent(part)) == kDateComponentInvalid ) { return; }

        auto tp_left  = to_time_point(left, true);
        auto tp_right = to_time_point(right, true);
        auto sign     = 1;
        if ( tp_left < tp_right ) {
            std::swap(tp_left, tp_right);
            std::swap(left, right);
            sign = -1;
        }

        const DateDiff diff{left.Y - right.Y,
                            doy(tp_left) - doy(tp_right),
                            left.h - right.h,
                            left.m - right.m,
                            static_cast<int64_t>(left.s) - static_cast<int64_t>(right.s),
                            static_cast<int64_t>((frac(left.s) - frac(right.s)) * 1000)};

        auto result = diffPart(left, right, diff, date_component);
        result *= sign;

        sqlite3_result_int64(ctx, result);
    }

    inline int64_t doDateAdd(sqlite3_context* ctx, const DateTime& start, const int64_t amount, const slice& part) {
        DateComponent date_component;
        if ( !part || (date_component = ParseDateComponent(part)) == kDateComponentInvalid ) { return -1; }

        year_month_day ymd = year(start.Y) / start.M / start.D;
        milliseconds   tod =
                hours(start.h) + minutes(start.m - start.tz) + milliseconds(static_cast<int64_t>(start.s * 1000));

        switch ( date_component ) {
            case kDateComponentMillisecond:
                tod += milliseconds(amount);
                break;
            case kDateComponentSecond:
                tod += seconds(amount);
                break;
            case kDateComponentMinute:
                tod += minutes(amount);
                break;
            case kDateComponentHour:
                tod += hours(amount);
                break;
            case kDateComponentDay:
                tod += days(amount);
                break;
            case kDateComponentWeek:
                tod += weeks(amount);
                break;
            case kDateComponentMonth:
                ymd += months(amount);
                break;
            case kDateComponentQuarter:
                ymd += quarters(amount);
                break;
            case kDateComponentYear:
                ymd += years(amount);
                break;
            case kDateComponentDecade:
                ymd += decades(amount);
                break;
            case kDateComponentCentury:
                ymd += centuries(amount);
                break;
            case kDateComponentMillennium:
                ymd += millenniums(amount);
                break;
            case kDateComponentInvalid:
                return -1;
        }

        return (sys_days(ymd) + tod).time_since_epoch().count();
    }

}  // namespace litecore
