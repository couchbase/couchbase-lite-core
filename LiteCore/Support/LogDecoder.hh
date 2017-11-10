//
//  LogDecoder.hh
//  Fleece
//
//  Created by Jens Alfke on 5/2/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include <iostream>
#include <map>
#include <vector>

namespace litecore {

    /** Decodes logs written by LogEncoder. */
    class LogDecoder {
    public:
        /** Initializes decoder with a stream written by a LogEncoder. */
        LogDecoder(std::istream&);

        /** Decodes the entire log and writes it to the output stream, with timestamps.
            If you want more control over the presentation, use the other methods below to
            read the timestamps and messages individually. */
        void decodeTo(std::ostream&, const std::vector<std::string> &levelNames);

        /** Reads the next line from the log, or returns false at EOF. */
        bool next();

        /** A timestamp, given as a standard time_t (seconds since 1/1/1970) plus a number of
            microseconds. */
        struct Timestamp {
            time_t secs;
            unsigned microsecs;
        };

        /** Returns the current line's timestamp. */
        Timestamp timestamp() const;

        /** Returns the current line's level. */
        int8_t level() const                    {return _curLevel;}

        /** Returns the current line's domain. */
        const std::string& domain() const       {return *_curDomain;}

        /** Reads the next message from the input and returns it as a string.
            You can only read each message once; calling this twice in a row will fail. */
        std::string readMessage();

        /** Reads the next message from the input and writes it to the output.
            You can only read each message once; calling this twice in a row will fail.  */
        void decodeMessageTo(std::ostream&);

        static Timestamp now();
        static void writeTimestamp(Timestamp, std::ostream&);
        static void writeHeader(const std::string &levelName,
                                const std::string &domainName,
                                std::ostream&);

    private:
        uint64_t readUVarInt();
        const std::string& readStringToken();
        std::string readCString();

        std::istream &_in;
        size_t _pointerSize;
        time_t _startTime;
        uint64_t _elapsedTicks {0};
        std::vector<std::string> _tokens;
        std::map<uint64_t,std::string> _objects;

        int8_t _curLevel {0};
        const std::string *_curDomain {nullptr};
        bool _readMessage;
    };

}
