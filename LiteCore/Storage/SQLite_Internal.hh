//
// SQLite_Internal.hh
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "DataFile.hh"
#include "fleece/slice.hh"
#include "Logging.hh"
#include <memory>

struct sqlite3;

namespace SQLite {
    class Database;
    class Statement;
    class Transaction;
}  // namespace SQLite

namespace fleece::impl {
    class SharedKeys;
}  // namespace fleece::impl

namespace litecore {

    extern LogDomain SQL;

    void LogStatement(const SQLite::Statement& st);


    constexpr const char* kWithDocBodiesCallbackPointerType = "WithDocBodiesCallback";

    // Little helper class that makes sure Statement objects get reset on exit
    class UsingStatement {
      public:
        explicit UsingStatement(SQLite::Statement& stmt) noexcept;

        explicit UsingStatement(const std::unique_ptr<SQLite::Statement>& stmt) noexcept : UsingStatement(*stmt) {}

        ~UsingStatement();

      private:
        SQLite::Statement& _stmt;
    };

    slice getColumnAsSlice(SQLite::Statement&, int col);

    // What the user_data of a registered function points to
    struct fleeceFuncContext {
        fleeceFuncContext(DataFile::Delegate* d, fleece::impl::SharedKeys* sk) : delegate(d), sharedKeys(sk) {}

        DataFile::Delegate*             delegate;
        fleece::impl::SharedKeys* const sharedKeys;
    };

    void RegisterSQLiteFunctions(sqlite3* db, fleeceFuncContext);
}  // namespace litecore
