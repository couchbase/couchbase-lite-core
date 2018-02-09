//
// SQLite_Internal.hh
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#pragma once
#include "DataFile.hh"
#include "Logging.hh"
#include <memory>

struct sqlite3;

namespace SQLite {
    class Database;
    class Statement;
    class Transaction;
}
namespace fleece {
    class SharedKeys;
}


namespace litecore {

    extern LogDomain SQL;

    void LogStatement(const SQLite::Statement &st);


    // Little helper class that makes sure Statement objects get reset on exit
    class UsingStatement {
    public:
        UsingStatement(SQLite::Statement &stmt) noexcept;

        UsingStatement(const std::unique_ptr<SQLite::Statement> &stmt) noexcept
        :UsingStatement(*stmt.get())
        { }

        ~UsingStatement();

    private:
        SQLite::Statement &_stmt;
    };


    void RegisterSQLiteFunctions(sqlite3 *db,
                                 DataFile::FleeceAccessor accessor,
                                 fleece::SharedKeys *sharedKeys);
}
