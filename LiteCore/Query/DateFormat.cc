#include "DateFormat.hh"
#include "ParseDate.hh"
#include "fleece/slice.hh"
#include <optional>
#include <slice_stream.hh>
#include <sstream>

namespace fleece {

    using namespace std::chrono;

    // The default format
    // YYYY-MM-DDThh:mm:ssTZD

    const DateFormat::YMD DateFormat::YMD::kISO8601 = YMD{Separator::Hyphen};

    const DateFormat::HMS DateFormat::HMS::kISO8601 = HMS{true, Separator::Colon};

    const DateFormat DateFormat::kISO8601 = DateFormat{YMD::kISO8601, Separator::T, HMS::kISO8601, {Timezone::NoColon}};

    /** This parses a subset of the formatting tokens of std::format
     * The valid tokens are:
     * %Y: Year (YYYY), %m: Month (MM), %d: Day (DD).
     * %F == %Y-%m-%d
     * %H: Hours (HH), %M: Minutes (MM), %S: Seconds (SS), %s: Milliseconds (sss) (ms is an addition to date.h tokens)
     * %T == %H:%M:%S.%s
     * %z: Timezone offset (Z for UTC, or offset in minutes (+/-)ZZZZ), %Ez: Timezone offset with colon ((+/-)ZZ:ZZ)
     * This parser is pretty restrictive, and only allows formats which match what we want.
     * YMD must always be full YMD, HMS must always be full HMS. Separators are restricted to 'T' and ' ' for YMD/HMS
     * separator, '-' and '/' for YMD separator, and ':' for HMS separator. Timezone offset is only allowed if HMS is
     * present. YMD can only be in ISO8601 order (no MDY allowed).
     *
     * ISO8601 can be represented as `%Y-%m-%dT%H:%M:%S%z` OR `%FT%T%z`
     * */
    std::optional<DateFormat> DateFormat::parseTokenFormat(slice_istream formatStream) {
        if ( formatStream.size < 2 ) return std::nullopt;

        // - YMD

        std::optional<YMD> ymd;

        // Skip past initial '%'
        formatStream.skip(1);

        // %F == %Y-%m-%d
        if ( const auto firstToken = formatStream.peekByte(); firstToken == 'F' ) {
            formatStream.skip(1);
            ymd = YMD::kISO8601;
        } else if ( firstToken == 'Y' ) {
            formatStream.skip(1);
            if ( const auto md = formatStream.readAtMost(6); md == "-%m-%d" ) {
                ymd = YMD::kISO8601;
            } else if ( md == "/%m/%d" ) {
                ymd                   = YMD::kISO8601;
                ymd.value().separator = YMD::Separator::Slash;
            } else {
                // If the first token is Y, we must have full valid YMD.
                return std::nullopt;
            }
        }

        if ( formatStream.empty() ) {
            if ( ymd.has_value() ) return DateFormat{ymd.value()};
            else
                return std::nullopt;
        }

        // - SEPARATOR

        std::optional<Separator> sep;

        if ( ymd.has_value() ) {
            switch ( formatStream.peekByte() ) {
                case (char)Separator::Space:
                    sep = Separator::Space;
                    formatStream.skip(1);
                    break;
                case (char)Separator::T:
                    sep = Separator::T;
                    formatStream.skip(1);
                    break;
                default:
                    sep = std::nullopt;
            }
            if ( formatStream.readByte() != '%' ) return std::nullopt;
        }

        if ( formatStream.size < 2 ) {
            if ( ymd.has_value() ) {
                return DateFormat{ymd.value()};
            } else
                return std::nullopt;
        }


        // - HMS

        std::optional<HMS> hms;

        // %T == %H:%M:%S
        if ( const auto firstToken = formatStream.readByte(); firstToken == 'T' ) {
            // ISO HMS, so we don't need to change `hms`
            hms = HMS::kISO8601;
        } else if ( firstToken == 'H' ) {
            if ( const auto ms = formatStream.readAtMost(6); ms == ":%M:%S" ) {
                hms = HMS::kISO8601;
            } else {
                return std::nullopt;
            }
            // Set Millis to None until we parse it later
            hms->millis = false;
        }

        if ( formatStream.size < 2 ) {
            if ( ymd.has_value() ) {
                if ( hms.has_value() ) {
                    // If YMD + HMS, Separator is required.
                    if ( !sep.has_value() ) { return std::nullopt; }
                    return DateFormat{ymd.value(), sep.value(), hms.value()};
                }
                return DateFormat{ymd.value()};
            }
            if ( hms.has_value() ) { return DateFormat{hms.value()}; }
            return std::nullopt;
        }

        if ( formatStream.readByte() != '%' ) return std::nullopt;

        // Millis
        // %s OR %.s
        if ( hms.has_value() ) {
            const auto ms = formatStream.peekByte();
            if ( ms == 's' ) {
                hms.value().millis = true;
                formatStream.skip(1);
            } else if ( ms == '.' && formatStream.readAtMost(2) == ".s" ) {
                hms.value().millis = true;
            }
        }

        if ( formatStream.size < 2 ) {
            if ( ymd.has_value() ) {
                if ( hms.has_value() ) {
                    // If YMD + HMS, Separator is required.
                    if ( !sep.has_value() ) { return std::nullopt; }
                    return DateFormat{ymd.value(), sep.value(), hms.value()};
                }
                return DateFormat{ymd.value()};
            }
            if ( hms.has_value() ) { return DateFormat{hms.value()}; }
            return std::nullopt;
        }

        // - TIMEZONE

        auto tz = kISO8601.tz;

        formatStream.readToDelimiter("%"_sl);
        const auto t = formatStream.readAtMost(2);

        if ( t == "z" ) {
            tz.value() = Timezone::NoColon;
        } else if ( t == "Ez" ) {
            tz.value() = Timezone::Colon;
        } else {
            // Format string contains additional invalid tokens
            return std::nullopt;
        }

        if ( ymd.has_value() ) {
            if ( hms.has_value() ) {
                // If YMD + HMS, Separator is required.
                if ( !sep.has_value() ) { return std::nullopt; }
                if ( tz.has_value() ) { return DateFormat{ymd.value(), sep.value(), hms.value(), tz.value()}; }
                return DateFormat{ymd.value(), sep.value(), hms.value()};
            }
            return DateFormat{ymd.value()};
        }
        if ( hms.has_value() ) {
            if ( tz.has_value() ) return DateFormat{hms.value(), tz.value()};
            return DateFormat{hms.value()};
        }
        return std::nullopt;
    }

    // 1111-11-11T11:11:11.111Z
    std::optional<DateFormat> DateFormat::parseDateFormat(slice formatString) {
        auto timezoneResult = parseTimezone(formatString);

        std::optional<Timezone> tzResult;
        if ( timezoneResult.has_value() ) {
            tzResult     = timezoneResult.value().first;
            formatString = timezoneResult.value().second;
        }

        auto hmsResult = parseHMS(formatString);

        if ( hmsResult.has_value() ) formatString = hmsResult.value().second;

        std::optional<Separator> separator;

        if ( !formatString.empty() && hmsResult.has_value() ) {
            char sep = (char)formatString[formatString.size - 1];
            if ( sep == (char)Separator::Space ) {
                separator = Separator::Space;
            } else if ( sep == (char)Separator::T ) {
                separator = Separator::T;
            } else {
                // Invalid YMD/HMS Separator
                return std::nullopt;
            }
            formatString = formatString.upTo(formatString.size - 1);
        }

        auto ymdResult = parseYMD(formatString);

        if ( separator.has_value() ) {
            // We must have YMD and HMS if there is a separator.
            if ( !ymdResult.has_value() || !hmsResult.has_value() ) { return std::nullopt; }
        }

        // We must have HMS if we have timezone specifier.
        if ( timezoneResult.has_value() && !hmsResult.has_value() ) { return std::nullopt; }

        if ( ymdResult.has_value() ) {
            if ( hmsResult.has_value() ) {
                if ( !separator.has_value() ) return std::nullopt;
                return {DateFormat{ymdResult.value(), separator.value(), hmsResult.value().first, tzResult}};
            }
            return {DateFormat{ymdResult.value()}};
        } else if ( hmsResult.has_value() ) {
            return {DateFormat{hmsResult.value().first, tzResult}};
        } else {
            // We must have _either_ YMD or HMS.
            return std::nullopt;
        }
    }

    std::optional<std::pair<DateFormat::Timezone, slice>> DateFormat::parseTimezone(const slice formatString) {
        // Default to No Colon
        if ( *(formatString.end() - 1) == 'Z' ) return {{Timezone::NoColon, formatString.upTo(formatString.size - 1)}};
        // Minimum 5 `+0000`
        if ( formatString.size < 5 ) return std::nullopt;
        const bool colon = *(formatString.end() - 3) == ':';

        const size_t start = colon ? formatString.size - 6 : formatString.size - 5;

        if ( formatString[start] == '+' || formatString[start] == '-' ) {
            if ( colon ) {
                return {{Timezone::Colon, formatString.upTo(start)}};
            } else {
                return {{Timezone::NoColon, formatString.upTo(start)}};
            }
        }

        return std::nullopt;
    }

    // Input some string which may or may not contain HMS but does NOT contain timezone. That should have already been
    // stripped by `parseTimezone` (ie "1111-11-11T11:11:11.111" or "11:11").
    // Returns the parsed HMS and the format string with HMS removed, or None if valid HMS was not found.
    std::optional<std::pair<DateFormat::HMS, slice>> DateFormat::parseHMS(slice formatString) {
        // Minimum 11:11:11
        if ( formatString.size < 8 ) return std::nullopt;
        const bool millis = *(formatString.end() - 4) == '.';

        // If we have millis, we must have minimum 11:11:11.111 (12 chars)
        if ( millis && formatString.size < 12 ) { return std::nullopt; }

        // Shorten to get rid of millis, input minimum is now 11:11:11
        if ( millis ) { formatString = formatString.upTo(formatString.size - 4); }

        // Check HMS is formatted correctly
        if ( !(*(formatString.end() - 3) == ':' && *(formatString.end() - 6) == ':') ) { return std::nullopt; }

        const size_t start = formatString.size - 8;

        return {{HMS{millis, HMS::Separator::Colon}, formatString.upTo(start)}};
    }

    // Input some string which may or may not contain YMD but does NOT contain HMS, Timezone, or the YMD/HMS separator.
    // This should be called after already calling `parseTimezone` ,`parseHMS`, and removing the separator.
    std::optional<DateFormat::YMD> DateFormat::parseYMD(slice formatString) {
        // Minimum 1111-11-11
        if ( formatString.size < 10 ) return std::nullopt;

        auto separator = YMD::Separator::Hyphen;

        if ( *(formatString.end() - 3) == '-' && *(formatString.end() - 6) == '-' ) {
        } else if ( *(formatString.end() - 3) == '/' && *(formatString.end() - 6) == '/' ) {
            separator = YMD::Separator::Slash;
        } else {
            return std::nullopt;
        }

        return {YMD{separator}};
    }

    std::optional<DateFormat> DateFormat::parse(const slice formatString) {
        if ( formatString.empty() ) { return std::nullopt; }
        if ( formatString[0] == '%' ) { return parseTokenFormat(formatString); }
        return parseDateFormat(formatString);
    }

    slice DateFormat::format(char buf[], const int64_t timestamp, const bool asUTC,
                             const std::optional<DateFormat> fmt) {
        if ( asUTC ) {
            return format(buf, timestamp, minutes{0}, fmt);
        } else {
            const milliseconds millis{timestamp};
            auto               temp           = FromTimestamp(floor<seconds>(millis));
            const seconds      offset_seconds = GetLocalTZOffset(&temp, false);
            return format(buf, timestamp, duration_cast<minutes>(offset_seconds), fmt);
        }
    }

    slice DateFormat::format(char buf[], const int64_t timestamp, const minutes tzoffset,
                             const std::optional<DateFormat> fmt) {
        if ( timestamp == kInvalidDate ) {
            *buf = 0;
            return nullslice;
        }

        std::ostringstream stream;

        const milliseconds millis{milliseconds{timestamp} + duration_cast<milliseconds>(tzoffset)};
        const auto         tm = local_time<milliseconds>{millis};

        const DateFormat f = fmt.has_value() ? fmt.value() : kISO8601;

        if ( f.ymd.has_value() ) { stream << std::format("{:%F}", tm); }

        if ( f.hms.has_value() ) {
            if ( f.ymd.has_value() ) { stream << (char)f.separator.value(); }

            if ( f.hms.value().millis && timestamp % 1000 ) {
                stream << std::format("{:%T}", tm);
            } else {
                const auto secs = duration_cast<seconds>(millis);
                stream << std::format("{:%T}", local_seconds(secs));
            }

            if ( f.tz.has_value() ) {
                if ( tzoffset.count() == 0 ) {
                    stream << 'Z';
                } else {
                    char     sign = tzoffset.count() < 0 ? '-' : '+';
                    hh_mm_ss hms{sign == '-' ? -tzoffset : tzoffset};
                    stream << std::format("{}{:02}", sign, hms.hours().count());
                    if ( f.tz.value() == Timezone::Colon ) stream << ":";
                    stream << std::format("{:02}", hms.minutes().count());
                }
            }
        }

        const std::string res = stream.str();
        strncpy(buf, res.c_str(), res.length());

        return {buf, res.length()};
    }

    bool DateFormat::YMD::operator==(const YMD& other) const { return separator == other.separator; }

    bool DateFormat::HMS::operator==(const HMS& other) const {
        return millis == other.millis && separator == other.separator;
    }

    bool DateFormat::operator==(const DateFormat& other) const {
        return ymd == other.ymd && hms == other.hms && separator == other.separator && tz == other.tz;
    }

    DateFormat::operator std::string() const {
        std::stringstream stream;
        if ( ymd.has_value() ) {
            const char sep = (char)ymd.value().separator;
            stream << "Y" << sep << "M" << sep << "D";
        }
        if ( separator.has_value() ) { stream << (char)separator.value(); }
        if ( hms.has_value() ) {
            const char sep = (char)hms.value().separator;
            stream << "h" << sep << "m" << sep << "s";
            if ( hms.value().millis ) { stream << ".SSS"; }
        }
        if ( tz.has_value() ) {
            if ( tz.value() == Timezone::Colon ) {
                stream << "Ez";
            } else {
                stream << "z";
            }
        }
        return stream.str();
    }

    std::ostream& operator<<(std::ostream& os, DateFormat const& df) {
        os << std::string(df);
        return os;
    }

    std::ostream& operator<<(std::ostream& os, std::optional<DateFormat> const& odf) {
        if ( odf.has_value() ) {
            os << std::string(odf.value());
        } else {
            os << "None";
        }
        return os;
    }

}  // namespace fleece
