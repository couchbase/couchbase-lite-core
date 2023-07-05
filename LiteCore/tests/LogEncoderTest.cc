//
// LogEncoderTest.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//


#include "LogEncoder.hh"
#include "LogDecoder.hh"
#include "LiteCoreTest.hh"
#include "StringUtil.hh"
#include "ParseDate.hh"
#include "fleece/PlatformCompat.hh"
#include <regex>
#include <sstream>
#include <fstream>

using namespace std;

#define DATESTAMP     "\\w+, \\d{2}/\\d{2}/\\d{2}"
#define DATESTAMP_UTC "\\w+ \\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z"
#define TIMESTAMP     "\\d{2}:\\d{2}:\\d{2}\\.\\d{6}\\| "

constexpr size_t kFolderBufSize = 64;

class LogObject : public Logging {
  public:
    explicit LogObject(const std::string& identifier) : Logging(DBLog), _identifier(identifier) {}

    explicit LogObject(std::string&& identifier) : Logging(DBLog), _identifier(identifier) {}

    void doLog(const char* format, ...) const __printflike(2, 3) { LOGBODY(Info); }

    std::string loggingClassName() const override { return _identifier; }

    unsigned getRef() const { return getObjectRef(); }

  private:
    std::string _identifier;
};

static string dumpLog(const string& encoded, const vector<string>& levelNames) {
    cerr << "Encoded log is " << encoded.size() << " bytes\n";
    stringstream in(encoded);
    stringstream outDecoded;
    LogDecoder   decoder(in);
    decoder.decodeTo(outDecoded, levelNames);
    string result = outDecoded.str();
    cerr << result;
    return result;
}

TEST_CASE("LogEncoder formatting", "[Log]") {
    // For checking the timestamp in the path to the binary log file.
#ifdef LITECORE_CPPTEST
    string logPath = litecore::createLogPath_forUnitTest(LogLevel::Info);
#endif
    stringstream out;
    {
        LogEncoder            logger(out, LogLevel::Info);
        size_t                size = 0xabcdabcd;
        map<unsigned, string> dummy;
        logger.log(nullptr, dummy, LogEncoder::None, "Unsigned %u, Long %lu, LongLong %llu, Size %zx, Pointer %p",
                   1234567890U, 2345678901LU, 123456789123456789LLU, size, (void*)0x7fff5fbc);
        for ( int sgn = -1; sgn <= 1; sgn += 2 ) {
            ptrdiff_t ptrdiff = 1234567890;
            logger.log(nullptr, dummy, LogEncoder::None, "Int %d, Long %ld, LongLong %lld, Size %zd, Char %c",
                       1234567890 * sgn, 234567890L * sgn, 123456789123456789LL * sgn, ptrdiff * sgn, '@');
        }
        const char* str = "C string";
        slice       buf("hello");
        logger.log(nullptr, dummy, LogEncoder::None, "String is '%s', slice is '%.*s' (hex %-.*s)", str, SPLAT(buf),
                   SPLAT(buf));
    }
    string encoded = out.str();
    string result  = dumpLog(encoded, {});

    regex expected(
            TIMESTAMP
            "---- Logging begins on " DATESTAMP_UTC " ----\\n" TIMESTAMP
            "Unsigned 1234567890, Long 2345678901, LongLong 123456789123456789, Size abcdabcd, Pointer "
            "0x7fff5fbc\\n" TIMESTAMP
            "Int -1234567890, Long -234567890, LongLong -123456789123456789, Size -1234567890, Char @\\n" TIMESTAMP
            "Int 1234567890, Long 234567890, LongLong 123456789123456789, Size 1234567890, Char @\\n" TIMESTAMP
            "String is 'C string', slice is 'hello' \\(hex 68656c6c6f\\)\\n");
    CHECK(regex_match(result, expected));

#ifdef LITECORE_CPPTEST
    // We insert timestamp in milliseconds (w.r.t. UTC) in the path to the binary log files.
    // We also add the timestamp inside the log. When decoded to string, it is
    // represented as UTC time, like, "Monday 2023-07-03T19:25:01Z"
    // We want to ensure they are consistent. The timestamp prepended to each line is in the local time
    // where the binary is decoded.
    regex  catchUTCTimeTag{"^" TIMESTAMP "---- Logging begins on (" DATESTAMP_UTC ")"};
    smatch m;
    REQUIRE(regex_search(result, m, catchUTCTimeTag));
    CHECK(m.size() == 2);
    string utcTimeTag = m[1].str();
    // Remove the weekday name
    REQUIRE(regex_search(utcTimeTag, m, regex{"[^0-9]*"}));
    string utctime           = m.suffix().str();
    auto   utctimestampInLog = fleece::ParseISO8601Date(slice(utctime));
    // From milliseconds to seconds
    utctimestampInLog /= 1000;

    REQUIRE(regex_search(logPath, m, regex{"cbl_info_([0-9]*)\\.cbllog$"}));
    string timestampOnLogFilePath = m[1].str();
    // chomp it to seconds
    REQUIRE(timestampOnLogFilePath.length() > 3);
    timestampOnLogFilePath = timestampOnLogFilePath.substr(0, timestampOnLogFilePath.length() - 3);

    stringstream ss;
    ss << utctimestampInLog;
    CHECK(ss.str() == timestampOnLogFilePath);
#endif
}

TEST_CASE("LogEncoder levels/domains", "[Log]") {
    static const vector<string> kLevels = {"***", "", "", "WARNING", "ERROR"};
    stringstream                out[4];
    {
        map<unsigned, string> dummy;
        LogEncoder            verbose(out[0], LogLevel::Verbose);
        LogEncoder            info(out[1], LogLevel::Info);
        LogEncoder            warning(out[2], LogLevel::Warning);
        LogEncoder            error(out[3], LogLevel::Error);
        info.log("Draw", dummy, LogEncoder::None, "drawing %d pictures", 2);
        verbose.log("Paint", dummy, LogEncoder::None, "Waiting for drawings");
        warning.log("Draw", dummy, LogEncoder::None, "made a mistake!");
        info.log("Draw", dummy, LogEncoder::None, "redrawing %d picture(s)", 1);
        info.log("Draw", dummy, LogEncoder::None, "Handing off to painter");
        info.log("Paint", dummy, LogEncoder::None, "Painting");
        error.log("Customer", dummy, LogEncoder::None, "This isn't what I asked for!");
    }

    static const vector<string> expectedDomains[] = {{"Paint"},
                                                     {"Draw", "Draw", "Draw", "Paint"},
                                                     {"Draw"},
                                                     {"Customer"}};

    for ( int i = 0; i < 4; i++ ) {
        string encoded = out[i].str();
        string result  = dumpLog(encoded, kLevels);

        stringstream in(encoded);
        LogDecoder   decoder(in);
        unsigned     j = 0;
        while ( decoder.next() ) {
            CHECK(decoder.level() == i + 1);
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
    string result  = dumpLog(encoded, {});
    regex  expected(TIMESTAMP "---- Logging begins on " DATESTAMP_UTC " ----\\n" TIMESTAMP
                              "\\{1\\|Tweedledum\\} I'm Tweedledum\\n" TIMESTAMP
                              "\\{3\\|Tweedledee\\} I'm Tweedledee\\n" TIMESTAMP
                              "\\{2\\|rattle\\} and I'm the rattle\\n");
    CHECK(regex_match(result, expected));

    encoded = out2.str();
    result  = dumpLog(encoded, {});

    // Confirm other encoders have the same ref for "rattle"
    expected = regex(TIMESTAMP "---- Logging begins on " DATESTAMP_UTC " ----\\n" TIMESTAMP
                               "\\{2\\|rattle\\} Am I the rattle too\\?\\n");
    CHECK(regex_match(result, expected));
}

TEST_CASE("LogEncoder auto-flush", "[Log]") {
    stringstream out;
    LogEncoder   logger(out, LogLevel::Info);
    logger.log(nullptr, map<unsigned, string>(), LogEncoder::None, "Hi there");

    logger.withStream([&](ostream& s) { CHECK(out.str().empty()); });
    string encoded;
    CHECK(WaitUntil(5000ms, [&out, &logger, &encoded] {
        logger.withStream([&](ostream& s) { encoded = out.str(); });
        return !encoded.empty();
    }));

    string result = dumpLog(encoded, {});
    CHECK(!result.empty());
}

TEST_CASE("Logging rollover", "[Log]") {
    auto now = chrono::milliseconds(time(nullptr));
    char folderName[kFolderBufSize];
    snprintf(folderName, kFolderBufSize, "Log_Rollover_%" PRIms "/", now.count());
    FilePath tmpLogDir = TestFixture::sTempDir[folderName];
    tmpLogDir.delRecursive();
    tmpLogDir.mkdir();
    tmpLogDir["intheway"].mkdir();

    {
        ofstream tmpOut(tmpLogDir["abcd"].canonicalPath(), ios::binary);
        tmpOut << "I" << endl;
    }


    const LogFileOptions prevOptions = LogDomain::currentLogFileOptions();
    LogFileOptions       fileOptions{tmpLogDir.canonicalPath(), LogLevel::Info, 1024, 1, false};
    LogDomain::writeEncodedLogsTo(fileOptions, "Hello");
    LogObject obj("dummy");
    for ( int i = 0; i < 1024; i++ ) {
        // Do a lot of logging, so that pruning also gets tested
        obj.doLog("This is line #%d in the log.", i);
        if ( i == 256 ) {
            // Otherwise the logging will happen to fast that
            // rollover won't have a chance to occur
            this_thread::sleep_for(2s);
        }
    }

    // HACK: Cause a flush so that the test has something in the second log
    // to actually read into the decoder
    snprintf(folderName, kFolderBufSize, "Log_Rollover2_%" PRIms "/", now.count());
    FilePath other = TestFixture::sTempDir[folderName];
    other.mkdir();
    LogFileOptions fileOptions2{other.canonicalPath(), LogLevel::Info, 1024, 2, false};
    LogDomain::writeEncodedLogsTo(fileOptions2, "Hello");

    vector<string> infoFiles;
    int            totalCount = 0;
    tmpLogDir.forEachFile([&infoFiles, &totalCount](const FilePath& f) {
        totalCount++;
        if ( f.path().find("info") != string::npos ) { infoFiles.push_back(f.path()); }
    });

    CHECK(totalCount == 8);  // 1 for each level besides info, 1 info, 1 "intheway", 1 "acbd"
    REQUIRE(infoFiles.size() == 2);
    stringstream out;
    ifstream     fin(infoFiles[0], ios::binary);
    LogDecoder   d1(fin);
    d1.decodeTo(out, vector<string>{"", "", "INFO", "", ""});

    out.str("");
    // If obj ref rollover is not working then this will throw an exception
    ifstream   fin2(infoFiles[1], ios::binary);
    LogDecoder d2(fin2);
    d2.decodeTo(out, vector<string>{"", "", "INFO", "", ""});

    LogDomain::writeEncodedLogsTo(prevOptions);  // undo writeEncodedLogsTo() call above
}

TEST_CASE("Logging throw in c++", "[Log]") {
    auto now = chrono::milliseconds(time(nullptr));
    char folderName[kFolderBufSize];
    snprintf(folderName, kFolderBufSize, "Log_Rollover_%" PRIms "/", now.count());
    FilePath       tmpLogDir = TestFixture::sTempDir[folderName];
    LogFileOptions fileOptions{tmpLogDir.path(), LogLevel::Info, 1024, 1, false};
    // Note that we haven't created tmpLogDir. Therefore, there will be an exception.
    string msg{"File Logger fails to open file, "};
    msg += tmpLogDir.path();
    string               excMsg;
    const LogFileOptions prevOptions = LogDomain::currentLogFileOptions();
    try {
        ExpectingExceptions x;
        LogDomain::writeEncodedLogsTo(fileOptions, "Hello");
    } catch ( std::exception& exc ) { excMsg = exc.what(); }
    CHECK(excMsg.find(msg) == 0);
    LogDomain::writeEncodedLogsTo(prevOptions);
}

TEST_CASE("Logging throw in c4", "[Log]") {
    auto now = chrono::milliseconds(time(nullptr));
    char folderName[kFolderBufSize];
    snprintf(folderName, kFolderBufSize, "Log_Rollover_%" PRIms "/", now.count());
    FilePath tmpLogDir = TestFixture::sTempDir[folderName];
    // Note that we haven't created tmpLogDir.
    C4Error        error;
    LogFileOptions prevOptions;
    {
        ExpectingExceptions x;
        prevOptions = LogDomain::currentLogFileOptions();
        CHECK(!c4log_writeToBinaryFile({kC4LogVerbose, slice(tmpLogDir.path()), 16 * 1024, 1, false}, &error));
    }
    string excMsg{"File Logger fails to open file, "};
    excMsg += tmpLogDir.path();
    string errMsg = "LiteCore CantOpenFile, \"";
    errMsg += excMsg;
    CHECK(string(c4error_getDescription(error)).find(errMsg) == 0);
    LogDomain::writeEncodedLogsTo(prevOptions);
}

TEST_CASE("Logging plaintext", "[Log]") {
    char folderName[kFolderBufSize];
    snprintf(folderName, kFolderBufSize, "Log_Plaintext_%" PRIms "/", chrono::milliseconds(time(nullptr)).count());
    FilePath tmpLogDir = TestFixture::sTempDir[folderName];
    tmpLogDir.delRecursive();
    tmpLogDir.mkdir();

    const LogFileOptions prevOptions = LogDomain::currentLogFileOptions();
    LogFileOptions       fileOptions{tmpLogDir.canonicalPath(), LogLevel::Info, 1024, 5, true};
    LogDomain::writeEncodedLogsTo(fileOptions, "Hello");
    LogObject obj("dummy");
    obj.doLog("This will be in plaintext");

    vector<string> infoFiles;
    tmpLogDir.forEachFile([&infoFiles](const FilePath& f) {
        if ( f.path().find("info") != string::npos ) { infoFiles.push_back(f.path()); }
    });

    REQUIRE(infoFiles.size() == 1);
    ifstream       fin(infoFiles[0]);
    string         line;
    vector<string> lines;
    while ( fin.good() ) {
        getline(fin, line);
        lines.push_back(line);
    }

    CHECK(lines[0] == "---- Hello ----");
    CHECK(lines[1].find("[DB]") != string::npos);
    CHECK(lines[1].find("{dummy#") != string::npos);
    CHECK(lines[1].find("This will be in plaintext") != string::npos);

    LogDomain::writeEncodedLogsTo(prevOptions);  // undo writeEncodedLogsTo() call above
}
