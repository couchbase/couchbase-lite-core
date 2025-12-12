//
// TextLogDecoder.hh
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "LogDecoder.hh"
#include "ParseDate.hh"
#include <istream>
#include <regex>
#include <stdexcept>

namespace litecore {

    /** Parses LiteCore-generated textual log files. */
    class TextLogDecoder : public LogIterator {
      public:
        /** Returns true if the stream `in` appears to contain textual log data. */
        static bool looksTextual(std::istream& in) {
            auto pos       = in.tellg();
            char chars[27] = {};
            bool result    = true;
            in.read((char*)chars, sizeof(chars));
            in.seekg(pos);
            return looksLikeLogLine(std::string_view(chars, std::size(chars)));
        }

        /** Initializes decoder with a stream written by LiteCore's textual log encoder. */
        explicit TextLogDecoder(std::istream& in) : _in(in) {
            _in.exceptions(std::istream::badbit);
            if ( next() && next() )  // Read header line to get the initial timestamp
                _startTime = _curTimestamp;
        }

        bool next() override {
            if ( _line.empty() ) {
                // Read next line if there's not one in the buffer:
                if ( !_in || _in.peek() < 0 ) return false;
                std::getline(_in, _line);
                if ( _line.empty() ) return false;
            }

            // Example: 2025-12-09T06:47:55.507699Z WS Verbose Obj=/JRepl@1175308903/…/ Received 58-byte message

            std::string_view rest(_line);
            auto             nextColumn = [&] {
                auto             next   = rest.find(' ');
                std::string_view column = rest.substr(0, next);
                rest                    = rest.substr(next + 1);
                return column;
            };

            auto timestamp = nextColumn();
            auto micros    = std::stoul(std::string(timestamp.substr(timestamp.size() - 7, 6)));
            auto millis    = fleece::ParseISO8601Date(timestamp);
            if ( millis == kInvalidDate || millis < 0x19000000 || micros > 999999 )
                throw std::runtime_error("Could not parse timestamp in log line:  " + _line);
            _curTimestamp = {millis / 1000, unsigned(micros)};

            _curDomain = nextColumn();

            auto levelStr = nextColumn();
            if ( auto i = std::ranges::find(kLevelNames, levelStr); i != std::end(kLevelNames) )
                _curLevel = i - std::begin(kLevelNames);
            else
                _curLevel = 0;

            _curObject.clear();
            _curObjectID = 0;
            if ( rest.starts_with("Obj=/") ) {
                std::string_view obj = nextColumn();
                if ( auto size = obj.size(); size >= 13 && obj.ends_with('/') ) {
                    if ( auto pos = obj.rfind('#'); pos != std::string::npos ) {
                        _curObject   = obj.substr(5, size - 6);  // trim 'Obj=/' and '/ '
                        _curObjectID = std::stoul(std::string(obj.substr(pos + 1, size - 2 - pos)));
                    }
                }
            }

            _curMessage = rest;

            // Add any following non-log-format lines to the message:
            _line.clear();
            while ( _in && _in.peek() >= 0 ) {
                std::getline(_in, _line);
                if ( _line.empty() ) break;
                if ( looksLikeLogLine(_line) ) break;
                _curMessage += '\n';
                _curMessage += _line;
                _line.clear();
            }

            return true;
        }

        Timestamp startTime() const override { return _startTime; }

        Timestamp timestamp() const override { return _curTimestamp; }

        int8_t level() const override { return _curLevel; }

        const std::string& domain() const override { return _curDomain; }

        uint64_t objectID() const override { return _curObjectID; }

        const std::string* objectDescription() const override { return &_curObject; }

        void decodeMessageTo(std::ostream& out) override { out << _curMessage; }

      private:
        static bool looksLikeLogLine(std::string_view line) {
            if ( line.size() < 27 ) return false;
            for ( uint8_t c : line.substr(0, 27) ) {
                if ( !isdigit(c) && c != '-' && c != ':' && c != '.' && c != 'Z' && c != 'T' ) return false;
            }
            return true;
        }

        static constexpr std::string_view kLevelNames[] = {"Debug", "Verbose", "Info", "WARNING", "ERROR"};

        std::istream& _in;
        Timestamp     _startTime{};
        std::string   _line;

        Timestamp   _curTimestamp;
        int8_t      _curLevel;
        std::string _curDomain;
        std::string _curObject;
        uint64_t    _curObjectID;
        std::string _curMessage;
    };

}  // namespace litecore
