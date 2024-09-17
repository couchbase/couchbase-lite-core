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
#include <thread>

using namespace std;

// These formats are used in the decoded log files. They are UTC times.
#define DATESTAMP "\\w+ \\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z"
#define TIMESTAMP "\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{6}Z"

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
        LogEncoder logger(out, int8_t(LogLevel::Info));
        size_t     size = 0xabcdabcd;
        logger.log(nullptr, "Unsigned %u, Long %lu, LongLong %llu, Size %zx, Pointer %p", 1234567890U, 2345678901LU,
                   123456789123456789LLU, size, (void*)0x7fff5fbc);
        for ( int sgn = -1; sgn <= 1; sgn += 2 ) {
            ptrdiff_t ptrdiff = 1234567890;
            logger.log(nullptr, "Int %d, Long %ld, LongLong %lld, Size %zd, Char %c", 1234567890 * sgn,
                       234567890L * sgn, 123456789123456789LL * sgn, ptrdiff * sgn, '@');
        }
        const char* str = "C string";
        slice       buf("hello");
        logger.log(nullptr, "String is '%s', slice is '%.*s' (hex %-.*s)", str, SPLAT(buf), SPLAT(buf));
    }
    string encoded = out.str();
    string result  = dumpLog(encoded, {});

    regex expected(
            TIMESTAMP
            " ---- Logging begins on " DATESTAMP " ----\\n" TIMESTAMP
            "   Unsigned 1234567890, Long 2345678901, LongLong 123456789123456789, Size abcdabcd, Pointer "
            "0x7fff5fbc\\n" TIMESTAMP "   Int -1234567890, Long -234567890, LongLong -123456789123456789, Size "
            "-1234567890, Char @\\n" TIMESTAMP
            "   Int 1234567890, Long 234567890, LongLong 123456789123456789, Size 1234567890, Char @\\n" TIMESTAMP
            "   String is 'C string', slice is 'hello' \\(hex 68656c6c6f\\)\\n");
    CHECK(regex_match(result, expected));

#ifdef LITECORE_CPPTEST
    // We insert timestamp in milliseconds (w.r.t. UTC) in the path to the binary log files.
    // We also add the timestamp inside the log. When decoded to string, it is
    // represented as UTC time, like, "Monday 2023-07-03T19:25:01Z"
    // We want to ensure they are consistent.
    regex  catchUTCTimeTag{"^" TIMESTAMP " ---- Logging begins on (" DATESTAMP ")"};
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
    C4LogDomain                 domainDraw;
    {
        // CBL-5726. LogDomain stores a copy of name string when created by c4log_domain.
        std::string draw{"Draw"};
        domainDraw = c4log_getDomain(draw.c_str(), true);
    }
    {
        LogEncoder verbose(out[0], int8_t(LogLevel::Verbose));
        LogEncoder info(out[1], int8_t(LogLevel::Info));
        LogEncoder warning(out[2], int8_t(LogLevel::Warning));
        LogEncoder error(out[3], int8_t(LogLevel::Error));
        info.log(c4log_getDomainName(domainDraw), "drawing %d pictures", 2);
        verbose.log("Paint", "Waiting for drawings");
        warning.log(c4log_getDomainName(domainDraw), "made a mistake!");
        info.log(c4log_getDomainName(domainDraw), "redrawing %d picture(s)", 1);
        info.log(c4log_getDomainName(domainDraw), "Handing off to painter");
        info.log("Paint", "Painting");
        error.log("Customer", "This isn't what I asked for!");
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
            ++j;
        }
    }
}

TEST_CASE("LogEncoder tokens", "[Log]") {
    LogDomain::ObjectMap objects;
    objects.emplace(1, make_pair("Tweedledum", 0));
    objects.emplace(2, make_pair("rattle", 1));
    objects.emplace(3, make_pair("Tweedledee", 2));

    stringstream out;
    stringstream out2;
    {
        LogEncoder logger(out, int8_t(LogLevel::Info));
        LogEncoder logger2(out2, int8_t(LogLevel::Verbose));
        logger.log(nullptr, LogEncoder::ObjectRef{1}, LogDomain::getObjectPath(LogEncoder::ObjectRef{1}, objects),
                   "I'm Tweedledum");
        logger.log(nullptr, LogEncoder::ObjectRef{3}, LogDomain::getObjectPath(LogEncoder::ObjectRef{3}, objects),
                   "I'm Tweedledee");
        logger.log(nullptr, LogEncoder::ObjectRef{2}, LogDomain::getObjectPath(LogEncoder::ObjectRef{2}, objects),
                   "and I'm the rattle");
        logger2.log(nullptr, LogEncoder::ObjectRef{2}, LogDomain::getObjectPath(LogEncoder::ObjectRef{2}, objects),
                    "Am I the rattle too?");
    }
    string encoded = out.str();
    string result  = dumpLog(encoded, {});
    regex  expected(TIMESTAMP " ---- Logging begins on " DATESTAMP " ----\\n" TIMESTAMP
                              "   Obj=/Tweedledum#1/ I'm Tweedledum\\n" TIMESTAMP
                              "   Obj=/Tweedledum#1/rattle#2/Tweedledee#3/ I'm Tweedledee\\n" TIMESTAMP
                              "   Obj=/Tweedledum#1/rattle#2/ and I'm the rattle\\n");
    CHECK(regex_match(result, expected));

    encoded = out2.str();
    result  = dumpLog(encoded, {});

    // Confirm other encoders have the same ref for "rattle"
    expected = regex(TIMESTAMP " ---- Logging begins on " DATESTAMP " ----\\n" TIMESTAMP
                               "   Obj=/Tweedledum#1/rattle#2/ Am I the rattle too\\?\\n");
    CHECK(regex_match(result, expected));
}

TEST_CASE("LogEncoder auto-flush", "[Log]") {
    stringstream out;
    LogEncoder   logger(out, int8_t(LogLevel::Info));
    logger.log(nullptr, "Hi there");

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

    int maxCount = 0;
    SECTION("No Purge") {
        // Allows 3 log files
        maxCount = 2;
    }
    SECTION("Purge old logs") {
        // Allows 2 log files.
        // The first one, with serialNo=1, will be purged.
        maxCount = 1;
    }

    LogFileOptions fileOptions{tmpLogDir.canonicalPath(), LogLevel::Info, 1024, maxCount, false};
#ifdef LITECORE_CPPTEST
    resetRotateSerialNo();
#endif
    LogDomain::writeEncodedLogsTo(fileOptions, "Hello");
    LogObject obj("dummy");
    // The following will trigger 2 rotations.
    for ( int i = 0; i < 1024; i++ ) {
        // Do a lot of logging, so that pruning also gets tested
        obj.doLog("This is line #%d in the log.", i);
        if ( i == 256 ) {
            // Otherwise the logging will happen to fast that
            // rollover won't have a chance to occur
            this_thread::sleep_for(1s);
        }
        if ( i == 256 * 2 ) {
            // To help to get 2 rotate events. If maxCount in the logOptions is 1,
            // one file will be purged from the disk. But we are guaranteed to
            // get 2 flush events on all platforms.
            this_thread::sleep_for(1s);
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

    // infoFiles.size(), log files at the Info level
    // 4 additional log files, 1 for each level besides Info.
    // 2 arbitrary files, "intheway" and "acbd", in particular
    REQUIRE(totalCount == infoFiles.size() + 6);
    // The rollover logic will cut a new file as its size reaches maxSize as specified in
    // the LogFileOptions. However, we check the size by checking the number of bytes already
    // flushed to the fstream. Therefore, the number of files that have actually been cut
    // depends on when flush gets called. No matter how many files are generated, the number
    // of files left on the disk is bounded by maxCount + 1.
    CHECK(infoFiles.size() <= maxCount + 1);

    vector<std::array<string, 5>> lines;
    auto                          getLines = [&](int f) {
        stringstream out;
        ifstream     fin(infoFiles[f], ios::binary);
        LogDecoder   d(fin);
        d.decodeTo(out, vector<string>{"", "", "INFO", "", ""});
        lines.push_back({});
        std::getline(out, lines.back()[0], '\n');  // header 1
        std::getline(out, lines.back()[1], '\n');  // header 2
        std::getline(out, lines.back()[2], '\n');  // initialMessage
        std::getline(out, lines.back()[3], '\n');  // first line of the log
        string last;
        while ( std::getline(out, last, '\n') ) {
            lines.back()[4] = last;  // last line of the log
        }
    };
    for ( int n = 0; n < infoFiles.size(); ++n ) {
        // If obj ref rollover is not working then this will throw an exception as n > 0
        getLines(n);
    }
    REQUIRE(lines.size() == infoFiles.size());

    auto findSerialNo = [&](int f) {
        regex  regxSerialNo{R"(serialNo=([1-9][0-9]*))"};
        smatch m;
        // line 1 includes the serialNo
        CHECK(regex_search(lines[f][1], m, regxSerialNo));
        REQUIRE(m.size() == 2);
        stringstream ss{m[1].str()};
        int          ret = 0;
        ss >> ret;
        // serialNo starts from 1
        REQUIRE(1 <= ret);
        return ret;
    };
    std::map<int, int> bySerialNo;
    for ( int n = 0; n < infoFiles.size(); ++n ) {
        // serialNo map to file index 0-2
        bySerialNo.emplace(findSerialNo(n), n);
    }
    CHECK(bySerialNo.size() == infoFiles.size());
    std::cout << "Number of Info log files = " << infoFiles.size() << ", max number is " << maxCount + 1 << ", "
              << bySerialNo.begin()->first - 1 << " files are dropped" << std::endl;

    //    Example outputs:
    //    ---------------
    //        for (int n = 0; n < 3; ++n) {
    //            if (bySerialNo[n] < 0) continue;
    //            for (const auto& s : lines[bySerialNo[n]]) {
    //                cout << s << endl;
    //            }
    //            cout << endl;
    //        }
    //    The above code outputs the following.
    //
    //    00:26:16.000000Z| ---- Logging begins on Thursday 2023-07-20T00:26:16Z ----
    //    00:26:16.138985Z| INFO: ---- serialNo=1,logDirectory=/private/tmp/LiteCore_Tests_1689812776/Log_Rollover_1689812776,fileLogLevel=2,fileMaxSize=1024,fileMaxCount=2 ----
    //    00:26:16.139006Z| INFO: ---- Hello ----
    //    00:26:16.139315Z| [DB] INFO: {1|dummy} This is line #0 in the log.
    //    00:26:18.146657Z| [DB] INFO: {1} This is line #257 in the log.
    //
    //    00:26:18.000000Z| ---- Logging begins on Thursday 2023-07-20T00:26:18Z ----
    //    00:26:18.147429Z| INFO: ---- serialNo=2,logDirectory=/private/tmp/LiteCore_Tests_1689812776/Log_Rollover_1689812776,fileLogLevel=2,fileMaxSize=1024,fileMaxCount=2 ----
    //    00:26:18.147463Z| INFO: ---- Hello ----
    //    00:26:18.147587Z| [DB] INFO: {1|dummy} This is line #258 in the log.
    //    00:26:20.152145Z| [DB] INFO: {1} This is line #513 in the log.
    //
    //    00:26:20.000000Z| ---- Logging begins on Thursday 2023-07-20T00:26:20Z ----
    //    00:26:20.153720Z| INFO: ---- serialNo=3,logDirectory=/private/tmp/LiteCore_Tests_1689812776/Log_Rollover_1689812776,fileLogLevel=2,fileMaxSize=1024,fileMaxCount=2 ----
    //    00:26:20.153786Z| INFO: ---- Hello ----
    //    00:26:20.153917Z| [DB] INFO: {1|dummy} This is line #514 in the log.
    //    00:26:20.160590Z| [DB] INFO: {1} This is line #1023 in the log.

    auto findLineNo = [&](int f) {
        regex  regxLineNo{R"(This is line #([0-9]*) in the log)"};
        smatch m;
        CHECK(regex_search(lines[f][3], m, regxLineNo));
        REQUIRE(m.size() == 2);
        stringstream s1    = stringstream{m[1].str()};
        int          begin = 0;
        s1 >> begin;

        CHECK(regex_search(lines[f][4], m, regxLineNo));
        REQUIRE(m.size() == 2);
        stringstream s2  = stringstream{m[1].str()};
        int          end = 0;
        s2 >> end;
        return std::make_pair(begin, end);
    };

    std::map<int, int[2]> lineNos;
    for ( const auto& s2f : bySerialNo ) {
        auto& ln               = lineNos[s2f.first];
        std::tie(ln[0], ln[1]) = findLineNo(s2f.second);
    }
    REQUIRE(lineNos.size() == bySerialNo.size());

    int lastLine = 0;
    for ( auto it = lineNos.begin(), prev = lineNos.end(); it != lineNos.end(); ++it ) {
        if ( prev == lineNos.end() ) {
            if ( it->first == 1 ) {
                // SerialNo == 1
                CHECK(it->second[0] == 0);
            }
            prev     = it;
            lastLine = it->second[1];
            continue;
        }
        CHECK(prev->first + 1 == it->first);          // SerialNos are sonsecutive
        CHECK(prev->second[1] + 1 == it->second[0]);  // line numbers are consecutive
        lastLine = it->second[1];
        prev     = it;
    }
    CHECK(lastLine == 1023);

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
#ifdef LITECORE_CPPTEST
    litecore::resetRotateSerialNo();
#endif
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

    int n = 0;
#ifdef LITECORE_CPPTEST
    regex checkHeader{
            TIMESTAMP
            R"(  Info ---- serialNo=1,logDirectory=[^,]*,fileLogLevel=2,fileMaxSize=1024,fileMaxCount=5 ----)"};
    smatch m;
    CHECK(regex_match(lines[n++], m, checkHeader));
#else
    n++;
#endif
    smatch m2;
    regex  checkLine1{TIMESTAMP "  Info ---- Hello ----"};
    CHECK(regex_match(lines[n++], m2, checkLine1));
    regex checkLine2{TIMESTAMP " DB Info Obj=/dummy#[0-9]+/ This will be in plaintext"};
    CHECK(regex_match(lines[n], m2, checkLine2));

    LogDomain::writeEncodedLogsTo(prevOptions);  // undo writeEncodedLogsTo() call above
}
