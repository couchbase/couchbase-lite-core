//
// LogObserverTest.hh
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Logging.hh"
#include "LogObserver.hh"
#include <queue>

using namespace std;
using namespace litecore;

/** Simple implementation of Logging for tests. */
class LogObject : public Logging {
  public:
    explicit LogObject(LogDomain& domain, const std::string& identifier) : Logging(domain), _identifier(identifier) {}

    explicit LogObject(const std::string& identifier) : LogObject(DBLog, identifier) {}

    void setKeyValuePairs(const std::string& kv) { _kv = kv; }

    void doLog(const char* format, ...) const __printflike(2, 3) { LOGBODY(Info); }

    std::string loggingClassName() const override { return _identifier; }

    std::string loggingKeyValuePairs() const override { return _kv; }

    LogObjectRef getRef() const { return getObjectRef(); }

  private:
    std::string _identifier, _kv;
};

/** Simple LogObserver that records every message it receives. */
struct LogRecorder : public LogObserver {
    void observe(LogEntry const& entry) noexcept { entries.push_back(entry); }

    vector<LogEntry> entries;

    string messages(size_t i) const { return entries.at(i).messageStr(); }
};
