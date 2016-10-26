//
//  SQLite_Internal.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/28/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
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

    // Little helper class that makes sure Statement objects get reset on exit
    class UsingStatement {
    public:
        UsingStatement(SQLite::Statement &stmt) noexcept
        :_stmt(stmt)
        { }

        UsingStatement(const std::unique_ptr<SQLite::Statement> &stmt) noexcept
        :UsingStatement(*stmt.get())
        { }

        ~UsingStatement();

    private:
        SQLite::Statement &_stmt;
    };


    int RegisterFleeceFunctions(sqlite3 *db, fleece::SharedKeys*);
    int RegisterFleeceEachFunctions(sqlite3 *db, fleece::SharedKeys*);

}
