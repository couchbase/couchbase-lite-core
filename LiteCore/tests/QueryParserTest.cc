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


static QueryParser parserWith(string whereJson, slice sortJson = nullslice) {
    QueryParser qp("kv_default");
    qp.parseJSON(slice(enquotify(whereJson)), sortJson);
    return qp;
}

static string parseWhere(string json) {
    return parserWith(json).whereClause();
}

static string parseSort(string sortJson) {
    return parserWith("{}", slice(enquotify(sortJson))).orderByClause();
}


TEST_CASE("QueryParser simple", "[Query]") {
    CHECK(parseWhere("{`name`: `Puddin' Tane`}")
          == "fl_value(body, 'name') = 'Puddin'' Tane'");
    CHECK(parseWhere("{`name`: `Puddin' Tane`, `again`: true}")
          == "fl_value(body, 'again') = 1 AND fl_value(body, 'name') = 'Puddin'' Tane'");
    CHECK(parseWhere("{`$and`: [{`name`: `Puddin' Tane`}, {`again`: true}]}")
          == "fl_value(body, 'name') = 'Puddin'' Tane' AND fl_value(body, 'again') = 1");
    CHECK(parseWhere("{`$nor`: [{`name`: `Puddin' Tane`}, {`again`: true}]}")
          == "NOT (fl_value(body, 'name') = 'Puddin'' Tane' OR fl_value(body, 'again') = 1)");

    CHECK(parseWhere("{`age`: {`$gte`: 21}}")
          == "fl_value(body, 'age') >= 21");
    CHECK(parseWhere("{`address`: {`state`: `CA`, `zip`: {`$lt`: 95000}}}")
          == "(fl_value(body, 'address.state') = 'CA' AND fl_value(body, 'address.zip') < 95000)");

    CHECK(parseWhere("{`name`: {`$exists`: true}}")
          == "fl_exists(body, 'name')");
    CHECK(parseWhere("{`name`: {`$exists`: false}}")
          == "NOT fl_exists(body, 'name')");

    CHECK(parseWhere("{`name`: {`$type`: `string`}}")
          == "fl_type(body, 'name')=3");

    CHECK(parseWhere("{`name`: {`$in`: [`Webbis`, `Wowbagger`]}}")
          == "fl_value(body, 'name') IN ('Webbis', 'Wowbagger')");
    CHECK(parseWhere("{`age`: {`$nin`: [6, 7, 8]}}")
          == "fl_value(body, 'age') NOT IN (6, 7, 8)");

    CHECK(parseWhere("{`coords`: {`$size`: 2}}")
          == "fl_count(body, 'coords')=2");

    CHECK(parseWhere("{`tags`: {`$all`: [`mind-bending`, `heartwarming`]}}")
          == "fl_contains(body, 'tags', 1, 'mind-bending', 'heartwarming')");
    CHECK(parseWhere("{`tags`: {`$any`: [`mind-bending`, `heartwarming`]}}")
          == "fl_contains(body, 'tags', 0, 'mind-bending', 'heartwarming')");

    CHECK(parseWhere("{`name`: [1]}")
          == "fl_value(body, 'name') = :_1");
    CHECK(parseWhere("{`name`: [`name`]}")
          == "fl_value(body, 'name') = :_name");
}


TEST_CASE("QueryParser bindings", "[Query]") {
    CHECK(parseWhere("{`age`: {`$gte`: [1]}}")
          == "fl_value(body, 'age') >= :_1");
    CHECK(parseWhere("{`address`: {`state`: [`state`], `zip`: {`$lt`: [`zip`]}}}")
          == "(fl_value(body, 'address.state') = :_state AND fl_value(body, 'address.zip') < :_zip)");
}


TEST_CASE("QueryParser sort", "[Query]") {
    CHECK(parseSort("[`size`]")
          == "fl_value(body, 'size')");
    CHECK(parseSort("[`+size`, `-price`]")
          == "fl_value(body, 'size'), fl_value(body, 'price') DESC");
    CHECK(parseSort("[`_id`, `-_sequence`]")
          == "key, sequence DESC");
}


TEST_CASE("QueryParser elemMatch", "[Query]") {
    CHECK(parseWhere("{`tags`: {`$elemMatch`: {`$eq`: `moist`}}}")
          == "EXISTS (SELECT 1 FROM fl_each(body, 'tags') WHERE fl_each.value = 'moist')");
    CHECK(parseWhere("{`prices`: {`$elemMatch`: {`$ge`: 3.95}}, "
                             "`tags`: {`$elemMatch`: {`$eq`: `moist`}}}")
          == "EXISTS (SELECT 1 FROM fl_each(body, 'prices') WHERE fl_each.value >= 3.95) AND EXISTS (SELECT 1 FROM fl_each(body, 'tags') WHERE fl_each.value = 'moist')");
}


TEST_CASE("QueryParser FTS", "[Query]") {
    auto p = parserWith("{`bio`: {`$match`: `architect`}}");
    CHECK(p.fromClause() == "kv_default, \"kv_default::bio\" AS FTS1");
    CHECK(p.whereClause() == "(FTS1.text MATCH 'architect' AND FTS1.rowid = kv_default.sequence)");

    p = parserWith("{`bio`: {`$match`: `architect`}, `skills`: {`$match`: `mobile`}}");
    CHECK(p.fromClause() == "kv_default, \"kv_default::bio\" AS FTS1, \"kv_default::skills\" AS FTS2");
    CHECK(p.whereClause() == "(FTS1.text MATCH 'architect' AND FTS1.rowid = kv_default.sequence) AND (FTS2.text MATCH 'mobile' AND FTS2.rowid = kv_default.sequence)");
}
