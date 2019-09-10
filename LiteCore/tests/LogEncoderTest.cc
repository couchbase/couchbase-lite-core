//
// LogEncoderTest.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//


#include "LogEncoder.hh"
#include "LogDecoder.hh"
#include "LiteCoreTest.hh"
#include "StringUtil.hh"
#include <regex>
#include <sstream>
#include <fstream>

#define DATESTAMP "\\w+, \\d{2}/\\d{2}/\\d{2}"
#define TIMESTAMP "\\d{2}:\\d{2}:\\d{2}\\.\\d{6}\\| "

class LogObject : public Logging
{
public:
    LogObject(const std::string& identifier)
        : Logging(DBLog)
    ,_identifier(identifier)
    {
        
    }

    LogObject(std::string&& identifier)
        : Logging(DBLog)
    ,_identifier(identifier)
    {
        
    }

    void doLog(const char *format, ...) const __printflike(2, 3) { LOGBODY(Info); }

    std::string loggingClassName() const override
    {
        return _identifier;
    }

    unsigned getRef() const
    {
        return getObjectRef();
    }
private:
    std::string _identifier;
};

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
        LogEncoder logger(out, LogLevel::Info);
        size_t size = 0xabcdabcd;
        map<unsigned, string> dummy;
        logger.log(nullptr, dummy, LogEncoder::None, "Unsigned %u, Long %lu, LongLong %llu, Size %zx, Pointer %p",
                   1234567890U, 2345678901LU, 123456789123456789LLU, size, (void*)0x7fff5fbc);
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            ptrdiff_t ptrdiff = 1234567890;
            logger.log(nullptr, dummy, LogEncoder::None, "Int %d, Long %ld, LongLong %lld, Size %zd, Char %c",
                       1234567890*sgn, 234567890L*sgn, 123456789123456789LL*sgn, ptrdiff*sgn, '@');
        }
        const char *str = "C string";
        slice buf("hello");
        logger.log(nullptr, dummy, LogEncoder::None, "String is '%s', slice is '%.*s' (hex %-.*s)", str, SPLAT(buf), SPLAT(buf));
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
    stringstream out[4];
    {
        map<unsigned, string> dummy;
        LogEncoder verbose(out[0], LogLevel::Verbose);
        LogEncoder info(out[1], LogLevel::Info);
        LogEncoder warning(out[2], LogLevel::Warning);
        LogEncoder error(out[3], LogLevel::Error);
        info.log("Draw", dummy, LogEncoder::None, "drawing %d pictures", 2);
        verbose.log("Paint", dummy, LogEncoder::None, "Waiting for drawings");
        warning.log("Draw", dummy, LogEncoder::None, "made a mistake!");
        info.log("Draw", dummy, LogEncoder::None, "redrawing %d picture(s)", 1);
        info.log("Draw", dummy, LogEncoder::None, "Handing off to painter");
        info.log("Paint", dummy, LogEncoder::None, "Painting");
        error.log("Customer", dummy, LogEncoder::None, "This isn't what I asked for!");
    }

    static const vector<string> expectedDomains[] = {
        { "Paint" },
        { "Draw", "Draw", "Draw", "Paint" },
        { "Draw" },
        { "Customer" }
    };
    
    for(int i = 0; i < 4; i++) {
        string encoded = out[i].str();
        string result = dumpLog(encoded, kLevels);

        stringstream in(encoded);
        LogDecoder decoder(in);
        unsigned j = 0;
        while (decoder.next()) {
            CHECK(decoder.level() == i+1);
            CHECK(string(decoder.domain()) == expectedDomains[i][j]);
            ++i;
        }
    }
}


TEST_CASE("LogEncoder tokens", "[Log]") {
    map<unsigned, string> objects;
    objects.emplace(make_pair(1, "Tweedledum"));
    objects.emplace(make_pair(2, "rattle"));
    objects.emplace(make_pair(3, "Tweedledee"));

    stringstream out;
    stringstream out2;
    {
        LogEncoder logger(out, LogLevel::Info);
        LogEncoder logger2(out2, LogLevel::Verbose);
        logger.log(nullptr, objects, (LogEncoder::ObjectRef)1, "I'm Tweedledum");
        logger.log(nullptr, objects, (LogEncoder::ObjectRef)3, "I'm Tweedledee");
        logger.log(nullptr, objects, (LogEncoder::ObjectRef)2, "and I'm the rattle");
        logger2.log(nullptr, objects, (LogEncoder::ObjectRef)2, "Am I the rattle too?");
    }
    string encoded = out.str();
    string result = dumpLog(encoded, {});
    regex expected(TIMESTAMP "---- Logging begins on " DATESTAMP " ----\\n"
                   TIMESTAMP "\\{1\\|Tweedledum\\} I'm Tweedledum\\n"
                   TIMESTAMP "\\{3\\|Tweedledee\\} I'm Tweedledee\\n"
                   TIMESTAMP "\\{2\\|rattle\\} and I'm the rattle\\n");
    CHECK(regex_match(result, expected));

    encoded = out2.str();
    result = dumpLog(encoded, {});

    // Confirm other encoders have the same ref for "rattle"
    expected = regex(TIMESTAMP "---- Logging begins on " DATESTAMP " ----\\n"
                   TIMESTAMP "\\{2\\|rattle\\} Am I the rattle too\\?\\n");
    CHECK(regex_match(result, expected));
}


TEST_CASE("LogEncoder auto-flush", "[Log]") {
    stringstream out;
    LogEncoder logger(out, LogLevel::Info);
    logger.log(nullptr, map<unsigned, string>(), LogEncoder::None, "Hi there");

    logger.withStream([&](ostream &s) {
        CHECK(out.str().empty());
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    string encoded;
    logger.withStream([&](ostream &s) {
        encoded = out.str();
    });

    CHECK(!encoded.empty());
    string result = dumpLog(encoded, {});
    CHECK(!result.empty());
}

TEST_CASE("Logging rollover", "[Log]") {
    auto now = chrono::milliseconds(time(nullptr));
    char folderName[64];
    sprintf(folderName, "Log_Rollover_%lld/", now.count());
    FilePath tmpLogDir = FilePath::tempDirectory()[folderName];
    tmpLogDir.delRecursive();
    tmpLogDir.mkdir();
    tmpLogDir["intheway"].mkdir();
    
    {
        ofstream tmpOut(tmpLogDir["abcd"].canonicalPath(), ios::binary);
        tmpOut << "I" << endl;
    }
    

    LogFileOptions fileOptions { tmpLogDir.canonicalPath(), LogLevel::Info, 1024, 1, false };
    LogDomain::writeEncodedLogsTo(fileOptions, "Hello");
    LogObject obj("dummy");
    for(int i = 0; i < 1024; i++) {
        // Do a lot of logging, so that pruning also gets tested
        obj.doLog("This is line #%d in the log.", i);
        if(i == 256) {
            // Otherwise the logging will happen to fast that
            // rollover won't have a chance to occur
            this_thread::sleep_for(chrono::seconds(2));
        }
    }

    // HACK: Cause a flush so that the test has something in the second log
    // to actually read into the decoder
    sprintf(folderName, "Log_Rollover2_%lld/", now.count());
    FilePath other = FilePath::tempDirectory()[folderName];
    other.mkdir();
    LogFileOptions fileOptions2 { other.canonicalPath(), LogLevel::Info, 1024, 2, false };
    LogDomain::writeEncodedLogsTo(fileOptions2, "Hello");
    
    vector<string> infoFiles;
    int totalCount = 0;
    tmpLogDir.forEachFile([&infoFiles, &totalCount](const FilePath f)
    {
       totalCount++;
       if(f.path().find("info") != string::npos) {
           infoFiles.push_back(f.path());
       } 
    });

    CHECK(totalCount == 8); // 1 for each level besides info, 1 info, 1 "intheway", 1 "acbd"
    REQUIRE(infoFiles.size() == 2);
    stringstream out;
    ifstream fin(infoFiles[0], ios::binary);
    LogDecoder d1(fin);
    d1.decodeTo(out, vector<string> { "", "", "INFO", "", "" });

    out.str("");
    // If obj ref rollover is not working then this will throw an exception
    ifstream fin2(infoFiles[1], ios::binary);
    LogDecoder d2(fin2);
    d2.decodeTo(out, vector<string> { "", "", "INFO", "", "" });
}

TEST_CASE("Logging plaintext", "[Log]") {
    char folderName[64];
    sprintf(folderName, "Log_Plaintext_%lld/", chrono::milliseconds(time(nullptr)).count());
    FilePath tmpLogDir = FilePath::tempDirectory()[folderName];
    tmpLogDir.delRecursive();
    tmpLogDir.mkdir();

    LogFileOptions fileOptions { tmpLogDir.canonicalPath(), LogLevel::Info, 1024, 5, true };
    LogDomain::writeEncodedLogsTo(fileOptions, "Hello");
    LogObject obj("dummy");
    obj.doLog("This will be in plaintext");

    vector<string> infoFiles;
    tmpLogDir.forEachFile([&infoFiles](const FilePath f)
    {
       if(f.path().find("info") != string::npos) {
           infoFiles.push_back(f.path());
       } 
    });

    REQUIRE(infoFiles.size() == 1);
    ifstream fin(infoFiles[0]);
    string line;
    vector<string> lines;
    while(fin.good()) {
        getline(fin, line);
        lines.push_back(line);
    }

    CHECK(lines[0] == "---- Hello ----");
    CHECK(lines[1].find("[DB]") != string::npos);
    CHECK(lines[1].find("{dummy#") != string::npos);
    CHECK(lines[1].find("This will be in plaintext") != string::npos);
}

