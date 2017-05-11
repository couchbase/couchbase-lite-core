//
//  LogEncoderTest.cc
//  Fleece
//
//  Created by Jens Alfke on 5/2/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//


#include "LogEncoder.hh"
#include "LogDecoder.hh"
#include "LiteCoreTest.hh"
#include "StringUtil.hh"
#include <regex>
#include <sstream>


#define DATESTAMP "\\w+, \\d{2}/\\d{2}/\\d{2}"
#define TIMESTAMP "\\d{2}:\\d{2}:\\d{2}\\.\\d{6}\\| "


static string dumpLog(string encoded, vector<string> levelNames) {
    cerr << "Encoded log is " << encoded.size() << " bytes\n";
    stringstream in(encoded);
    stringstream outDecoded;
    LogDecoder decoder(in);
    decoder.decodeTo(outDecoded, levelNames);
    string result = outDecoded.str();
    cerr << result;
    return result;
}


TEST_CASE("LogEncoder formatting", "[Log]") {
    stringstream out;
    {
        LogEncoder logger(out);
        size_t size = 0xabcdabcd;
        logger.log(0, nullptr, LogEncoder::None, "Unsigned %u, Long %lu, LongLong %llu, Size %zx, Pointer %p",
                   1234567890U, 2345678901LU, 123456789123456789LLU, size, (void*)0x7fff5fbc);
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            ptrdiff_t ptrdiff = 1234567890;
            logger.log(0, nullptr, LogEncoder::None, "Int %d, Long %ld, LongLong %lld, Size %zd, Char %c",
                       1234567890*sgn, 234567890L*sgn, 123456789123456789LL*sgn, ptrdiff*sgn, '@');
        }
        const char *str = "C string";
        slice buf("hello");
        logger.log(0, nullptr, LogEncoder::None, "String is '%s', slice is '%.*s' (hex %-.*s)", str, SPLAT(buf), SPLAT(buf));
    }
    string encoded = out.str();
    string result = dumpLog(encoded, {});

    regex expected(TIMESTAMP "---- Logging begins on " DATESTAMP " ----\\n"
                   TIMESTAMP "Unsigned 1234567890, Long 2345678901, LongLong 123456789123456789, Size abcdabcd, Pointer 0x7fff5fbc\\n"
                   TIMESTAMP "Int -1234567890, Long -234567890, LongLong -123456789123456789, Size -1234567890, Char @\\n"
                   TIMESTAMP "Int 1234567890, Long 234567890, LongLong 123456789123456789, Size 1234567890, Char @\\n"
                   TIMESTAMP "String is 'C string', slice is 'hello' \\(hex 68656c6c6f\\)\\n");
    CHECK(regex_match(result, expected));
}


TEST_CASE("LogEncoder levels/domains", "[Log]") {
    static const vector<string> kLevels = {"***", "", "", "WARNING", "ERROR"};
    stringstream out;
    {
        LogEncoder logger(out);
        logger.log(2, "Draw", LogEncoder::None, "drawing %d pictures", 2);
        logger.log(1, "Paint", LogEncoder::None, "Waiting for drawings");
        logger.log(3, "Draw", LogEncoder::None, "made a mistake!");
        logger.log(2, "Draw", LogEncoder::None, "redrawing %d picture(s)", 1);
        logger.log(2, "Draw", LogEncoder::None, "Handing off to painter");
        logger.log(2, "Paint", LogEncoder::None, "Painting");
        logger.log(4, "Customer", LogEncoder::None, "This isn't what I asked for!");
    }

    string encoded = out.str();
    string result = dumpLog(encoded, kLevels);

    stringstream in(encoded);

    static const vector<int8_t> expectedLevel = {2, 1, 3, 2, 2, 2, 4};
    static const vector<string> expectedDomain = {"Draw", "Paint", "Draw", "Draw", "Draw", "Paint", "Customer"};
    unsigned i = 0;
    LogDecoder decoder(in);
    while (decoder.next()) {
        CHECK(decoder.level() == expectedLevel[i]);
        CHECK(string(decoder.domain()) == expectedDomain[i]);
        ++i;
    }
}
