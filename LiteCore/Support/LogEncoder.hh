//
//  LogEncoder.hh
//  Fleece
//
//  Created by Jens Alfke on 5/2/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Writer.hh"
#include "Stopwatch.hh"
#include "PlatformCompat.hh"
#include <stdarg.h>
#include <iostream>
#include <unordered_map>

namespace litecore {

    /** A very fast & compact logging service.
        The output is written in a binary format to avoid the CPU and space overhead of converting
        everything to ASCII. It can be decoded by the LogDecoder class. */
    class LogEncoder {
    public:
        LogEncoder(std::ostream &out);
        ~LogEncoder();

        enum ObjectRef : unsigned {
            None = 0
        };
        
        void vlog(int8_t level, const char *domain, ObjectRef, const char *format, va_list args);

        void log(int8_t level, const char *domain, ObjectRef, const char *format, ...) __printflike(5, 6);

        void flush();

        ObjectRef registerObject(std::string description);
        void unregisterObject(ObjectRef);

        /** A timestamp, given as a standard time_t (seconds since 1/1/1970) plus microseconds. */
        struct Timestamp {
            time_t secs;
            unsigned microsecs;
        };

    private:
        friend class LogDecoder;
        
        void writeUVarInt(uint64_t);
        void writeStringToken(const char *token);

        static const uint8_t kMagicNumber[4];
        static constexpr uint8_t kFormatVersion = 1;

        fleece::Writer _writer;
        std::ostream &_out;
        fleece::Stopwatch _st;
        int64_t _lastElapsed {0};
        int64_t _lastSaved {0};
        std::unordered_map<size_t, unsigned> _formats;
        std::unordered_map<unsigned, std::string> _objects;
        ObjectRef _lastObjectRef {ObjectRef::None};
    };

}
