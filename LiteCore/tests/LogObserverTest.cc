//
// LogObserverTest.cc
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "LogObserverTest.hh"
#include "LogObserver.hh"
#include "LiteCoreTest.hh"
#include <queue>
#include <regex>

using namespace std;

struct LogObserverTest {
    LogObserverTest() {
        _oldDBLevel = DBLog.level();
        DBLog.setLevel(LogLevel::Verbose);
        _oldSyncLevel = SyncLog.level();
        SyncLog.setLevel(LogLevel::Verbose);
    }

    ~LogObserverTest() {
        DBLog.setLevel(_oldDBLevel);
        SyncLog.setLevel(_oldSyncLevel);
        for ( auto& r : recorders ) LogObserver::remove(r);
    }

    template <class T = LogRecorder>
    Retained<T> newRecorder() {
        auto r = make_retained<T>();
        recorders.push_back(r);
        return r;
    }

    vector<Retained<LogRecorder>> recorders;
    LogLevel                      _oldDBLevel, _oldSyncLevel;
};

TEST_CASE_METHOD(LogObserverTest, "LogObserver", "[Log]") {
    auto verbose = newRecorder();
    LogObserver::add(verbose, LogLevel::Verbose);
    auto warning = newRecorder();
    LogObserver::add(warning, LogLevel::Warning);

    const char* longish =
            "This is a somewhat lengthy string that we are going to use to test how logging works with long messages. ";

    LogToAt(kC4Cpp_DefaultLog, Info, "this is default/info");
    LogToAt(DBLog, Verbose, "this is db/verbose");
    LogToAt(QueryLog, Warning, "this is query/warning");
    LogToAt(SyncLog, Error, "this is sync/error. %s%s%s%s%s", longish, longish, longish, longish, longish);

    CHECK(verbose->entries.size() == 4);
    CHECK(verbose->messages(0) == "this is default/info");
    CHECK(verbose->messages(1) == "this is db/verbose");
    CHECK(verbose->messages(2) == "this is query/warning");
    CHECK(verbose->messages(3) == "this is sync/error. "s + longish + longish + longish + longish + longish);

    REQUIRE(warning->entries.size() == 2);
    CHECK(warning->messages(0) == "this is query/warning");
    CHECK(warning->entries[0].level == LogLevel::Warning);
    CHECK(&warning->entries[0].domain == &QueryLog);

    CHECK(warning->messages(1) == "this is sync/error. "s + longish + longish + longish + longish + longish);
    CHECK(warning->entries[1].level == LogLevel::Error);
    CHECK(&warning->entries[1].domain == &SyncLog);
}

TEST_CASE_METHOD(LogObserverTest, "LogObserver Custom Domains", "[Log]") {
    auto                                    recorder = newRecorder();
    vector<std::pair<LogDomain&, LogLevel>> domains  = {{DBLog, LogLevel::Verbose}, {SyncLog, LogLevel::Info}};
    LogObserver::add(recorder, LogLevel::Warning, domains);

    LogToAt(kC4Cpp_DefaultLog, Info, "this is default/info");  // not recorded
    LogToAt(DBLog, Verbose, "this is db/verbose");
    LogToAt(QueryLog, Warning, "this is query/warning");
    LogToAt(SyncLog, Verbose, "this is sync/verbose");  // not recorded
    LogToAt(SyncLog, Info, "this is sync/info");

    REQUIRE(recorder->entries.size() == 3);
    CHECK(recorder->messages(0) == "this is db/verbose");
    CHECK(recorder->messages(1) == "this is query/warning");
    CHECK(recorder->messages(2) == "this is sync/info");
}

TEST_CASE_METHOD(LogObserverTest, "LogObserver Logging Objects", "[Log]") {
    auto recorder = newRecorder();
    LogObserver::add(recorder, LogLevel::Info);

    LogObject obj("LogObject");
    obj.doLog("hi from log object");
    obj.doLog("goodbye from log object");

    REQUIRE(recorder->entries.size() == 3);
    UNSCOPED_INFO(recorder->messages(0));
    CHECK(regex_match(recorder->messages(0), regex(R"(^\{LogObject#\d+\}==> (class )?LogObject \w+ @\w+$)")));
    UNSCOPED_INFO(recorder->messages(1));
    CHECK(regex_match(recorder->messages(1), regex(R"(^Obj=/LogObject#\d+/ hi from log object$)")));
    UNSCOPED_INFO(recorder->messages(2));
    CHECK(regex_match(recorder->messages(2), regex(R"(^Obj=/LogObject#\d+/ goodbye from log object$)")));
}

TEST_CASE_METHOD(LogObserverTest, "LogObserver KV Logging Objects", "[Log]") {
    auto recorder = newRecorder();
    LogObserver::add(recorder, LogLevel::Info);

    LogObject kvObj("LogObject");
    kvObj.setKeyValuePairs("energy=low");
    kvObj.doLog("hi from kv object");
    kvObj.setKeyValuePairs("energy=over9000");
    kvObj.doLog("goodbye from kv object");

    REQUIRE(recorder->entries.size() == 3);
    UNSCOPED_INFO(recorder->messages(0));
    CHECK(regex_match(recorder->messages(0), regex(R"(^\{LogObject#\d+\}==> (class )?LogObject \w+ @\w+$)")));
    UNSCOPED_INFO(recorder->messages(1));
    CHECK(regex_match(recorder->messages(1), regex(R"(^Obj=/LogObject#\d+/ energy=low hi from kv object$)")));
    UNSCOPED_INFO(recorder->messages(2));
    CHECK(regex_match(recorder->messages(2), regex(R"(^Obj=/LogObject#\d+/ energy=over9000 goodbye from kv object$)")));
}

struct ReentrantLogRecorder : public LogRecorder {
    void observe(LogEntry const& entry) noexcept override {
        LogRecorder::observe(entry);
        if ( entry.level < LogLevel::Warning )
            LogToAt(kC4Cpp_DefaultLog, Warning, "logged from %s!", "within the callback");
    }
};

TEST_CASE_METHOD(LogObserverTest, "Reentrant log calls", "[Log]") {
    auto recorder = newRecorder<ReentrantLogRecorder>();
    LogObserver::add(recorder, LogLevel::Info);

    LogToAt(kC4Cpp_DefaultLog, Info, "this is %s", "default/info");

    REQUIRE(recorder->entries.size() == 2);
    CHECK(recorder->messages(0) == "this is default/info");
    CHECK(recorder->messages(1) == "logged from within the callback!");
}
