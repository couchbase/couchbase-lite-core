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

    /// Logger for SQL related activity.
    extern LogDomain SQL;

    /// Logs the statement to the `SQL` logger at Info level.
    void LogStatement(const SQLite::Statement& st);

    /** Little helper class that resets a long-lived Statement and unbinds its parameters on exit.
        Otherwise, if the statement hasn't reached its last row it remains active,
        and using it again would cause an error. Clearing parameters may free up memory,
        and eliminates dangling pointers if `bindNoCopy` was used.

        This class is not needed with temporary Statement objects.

        As a bonus, the constructor calls `LogStatement()`. */
    class UsingStatement {
      public:
        explicit UsingStatement(SQLite::Statement& stmt) noexcept;

        explicit UsingStatement(const std::unique_ptr<SQLite::Statement>& stmt) noexcept : UsingStatement(*stmt) {}

        /// Destructor calls reset() and clearBindings(), catching any exceptions.
        ~UsingStatement() noexcept;

      private:
        SQLite::Statement& _stmt;
    };

    /// Returns the (uncopied) value of a text or blob column as a slice.
    slice getColumnAsSlice(SQLite::Statement&, int col);

    /** What the user_data of a registered SQL function points to. */
    struct fleeceFuncContext {
        fleeceFuncContext(DataFile::Delegate* d, fleece::impl::SharedKeys* sk) : delegate(d), sharedKeys(sk) {}

        DataFile::Delegate*             delegate;
        fleece::impl::SharedKeys* const sharedKeys;
    };

    /// Registers all our SQL functions. Called when opening a database.
    void RegisterSQLiteFunctions(sqlite3* db, fleeceFuncContext);

    // used by `SQLiteKeyStore::withDocBodies` and the `fl_callback` SQL function.
    constexpr const char* kWithDocBodiesCallbackPointerType = "WithDocBodiesCallback";
}  // namespace litecore
