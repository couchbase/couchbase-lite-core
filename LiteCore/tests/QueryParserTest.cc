//
//  QueryParserTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "QueryParser.hh"
#include "Fleece.hh"
#include <string>
#include <vector>
#include <iostream>
#include "LiteCoreTest.hh"

using namespace std;


static string parse(string json) {
    for (auto i = json.begin(); i != json.end(); ++i) {
        if (*i == '`')
            *i = '"';
    }
    auto fleeceData = JSONConverter::convertJSON(slice(json));
    const Value *root = Value::fromTrustedData(fleeceData);

    stringstream sql;
    QueryParser p(sql);
    p.parse(root);
    return sql.str();
}


TEST_CASE("QueryParser simple", "[Query]") {
    CHECK(parse("{`name`: `Puddin' Tane`}")
          == "fl_value(body, 'name') = 'Puddin'' Tane'");
    CHECK(parse("{`name`: `Puddin' Tane`, `again`: true}")
          == "fl_value(body, 'again') = 1 AND fl_value(body, 'name') = 'Puddin'' Tane'");
    CHECK(parse("{`$and`: [{`name`: `Puddin' Tane`}, {`again`: true}]}")
          == "fl_value(body, 'name') = 'Puddin'' Tane' AND fl_value(body, 'again') = 1");
    CHECK(parse("{`$nor`: [{`name`: `Puddin' Tane`}, {`again`: true}]}")
          == "NOT (fl_value(body, 'name') = 'Puddin'' Tane' OR fl_value(body, 'again') = 1)");

    CHECK(parse("{`age`: {`$gte`: 21}}")
          == "fl_value(body, 'age') >= 21");
    CHECK(parse("{`address`: {`state`: `CA`, `zip`: {`$lt`: 95000}}}")
          == "(fl_value(body, 'address.state') = 'CA' AND fl_value(body, 'address.zip') < 95000)");

    CHECK(parse("{`name`: {`$exists`: true}}")
          == "fl_exists(body, 'name')");
    CHECK(parse("{`name`: {`$exists`: false}}")
          == "NOT fl_exists(body, 'name')");

    CHECK(parse("{`name`: {`$type`: `string`}}")
          == "fl_type(body, 'name')=3");

    CHECK(parse("{`name`: {`$in`: [`Webbis`, `Wowbagger`]}}")
          == "fl_value(body, 'name') IN ('Webbis', 'Wowbagger')");
    CHECK(parse("{`age`: {`$nin`: [6, 7, 8]}}")
          == "fl_value(body, 'age') NOT IN (6, 7, 8)");

    CHECK(parse("{`coords`: {`$size`: 2}}")
          == "fl_count(body, 'coords')=2");

    CHECK(parse("{`tags`: {`$all`: [`mind-bending`, `heartwarming`]}}")
          == "fl_contains(body, 'tags', 1, 'mind-bending', 'heartwarming')");
    CHECK(parse("{`tags`: {`$any`: [`mind-bending`, `heartwarming`]}}")
          == "fl_contains(body, 'tags', 0, 'mind-bending', 'heartwarming')");

    CHECK(parse("{`name`: [1]}")
          == "fl_value(body, 'name') = :_1");
    CHECK(parse("{`name`: [`name`]}")
          == "fl_value(body, 'name') = :_name");
}
