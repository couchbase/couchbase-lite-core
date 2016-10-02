//
//  SQLiteFleeceFunctions.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/28/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once

struct sqlite3;

namespace litecore {

    int RegisterFleeceFunctions(sqlite3 *db);

}
