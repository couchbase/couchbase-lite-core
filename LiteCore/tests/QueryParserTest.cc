//
//  QueryParserTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "QueryParser.hh"
#include "Error.hh"
#include "Fleece.hh"
#include <string>
#include <vector>
#include <iostream>
#include "LiteCoreTest.hh"

using namespace std;


static string parse(string json) {
    QueryParser qp("kv_default");
    alloc_slice fleece = JSONConverter::convertJSON(json5(json));
    qp.parse(Value::fromTrustedData(fleece));
    return qp.SQL();
}

static string parseWhere(string json) {
    QueryParser qp("kv_default");
    alloc_slice fleece = JSONConverter::convertJSON(json5(json));
    qp.parseJustExpression(Value::fromTrustedData(fleece));
    return qp.SQL();
}

static void mustFail(string json) {
    QueryParser qp("kv_default");
    alloc_slice fleece = JSONConverter::convertJSON(json5(json));
    ExpectException(error::LiteCore, error::InvalidQuery, [&]{
        qp.parseJustExpression(Value::fromTrustedData(fleece));
    });
}


TEST_CASE("QueryParser basic", "[Query]") {
    CHECK(parseWhere("['=', ['.', 'name'], 'Puddin\\' Tane']")
          == "fl_value(body, 'name') = 'Puddin'' Tane'");
    CHECK(parseWhere("['=', ['.name'], 'Puddin\\' Tane']")
          == "fl_value(body, 'name') = 'Puddin'' Tane'");
    CHECK(parseWhere("['AND', ['=', ['.', 'again'], true], ['=', ['.', 'name'], 'Puddin\\' Tane']]")
          == "fl_value(body, 'again') = 1 AND fl_value(body, 'name') = 'Puddin'' Tane'");
    CHECK(parseWhere("['=', ['+', 2, 2], 5]")
          == "2 + 2 = 5");
    CHECK(parseWhere("['=', ['power()', 25, ['/', 1, 2]], 5]")
          == "power(25, 1 / 2) = 5");
    CHECK(parseWhere("['=', ['POWER()', 25, ['/', 1, 2]], 5]")
          == "power(25, 1 / 2) = 5");
    CHECK(parseWhere("['NOT', ['<', 2, 1]]")
          == "NOT (2 < 1)");
    CHECK(parseWhere("['-', ['+', 2, 1]]")
          == "-(2 + 1)");
    CHECK(parseWhere("['*', ['+', 1, 2], ['+', 3, ['-', 4]]]")
          == "(1 + 2) * (3 + -4)");
    CHECK(parseWhere("['*', ['+', 1, 2], ['-', ['+', 3, 4]]]")
          == "(1 + 2) * -(3 + 4)");
    CHECK(parseWhere("['BETWEEN', 10, 0, 100]")
          == "10 BETWEEN 0 AND 100");
    CHECK(parseWhere("['IN', ['.', 'name'], 'Webbis', 'Wowbagger']")
          == "fl_value(body, 'name') IN ('Webbis', 'Wowbagger')");
    CHECK(parseWhere("['NOT IN', ['.', 'age'], 6, 7, 8]")
          == "fl_value(body, 'age') NOT IN (6, 7, 8)");
    CHECK(parseWhere("['.', 'addresses', [1], 'zip']")
          == "fl_value(body, 'addresses[1].zip')");
    CHECK(parseWhere("['.', 'addresses', [1], 'zip']")
          == "fl_value(body, 'addresses[1].zip')");
}


TEST_CASE("QueryParser bindings", "[Query]") {
    CHECK(parseWhere("['=', ['$', 'X'], ['$', 7]]")
          == "$_X = $_7");
    CHECK(parseWhere("['=', ['$X'], ['$', 7]]")
          == "$_X = $_7");
}


TEST_CASE("QueryParser special properties", "[Query]") {
    CHECK(parseWhere("['ifnull()', ['.', '_id'], ['.', '_sequence']]")
          == "ifnull(key, sequence)");
    CHECK(parseWhere("['ifnull()', ['._id'], ['.', '_sequence']]")
          == "ifnull(key, sequence)");
}


TEST_CASE("QueryParser property contexts", "[Query]") {
    // Special cases where a property access uses a different function than fl_value()
    CHECK(parseWhere("['EXISTS', 17]")
          == "EXISTS 17");
    CHECK(parseWhere("['EXISTS', ['.', 'addresses']]")
          == "fl_exists(body, 'addresses')");
    CHECK(parseWhere("['EXISTS', ['.addresses']]")
          == "fl_exists(body, 'addresses')");
    CHECK(parseWhere("['array_count()', ['$', 'X']]")
          == "array_count($_X)");
    CHECK(parseWhere("['array_count()', ['.', 'addresses']]")
          == "fl_count(body, 'addresses')");
    CHECK(parseWhere("['array_count()', ['.addresses']]")
          == "fl_count(body, 'addresses')");
}


TEST_CASE("QueryParser ANY", "[Query]") {
    CHECK(parseWhere("['ANY', 'X', ['.', 'names'], ['=', ['?', 'X'], 'Smith']]")
          == "EXISTS (SELECT 1 FROM fl_each(body, 'names') AS _X WHERE _X.value = 'Smith')");
    CHECK(parseWhere("['EVERY', 'X', ['.', 'names'], ['=', ['?', 'X'], 'Smith']]")
          == "NOT EXISTS (SELECT 1 FROM fl_each(body, 'names') AS _X WHERE NOT (_X.value = 'Smith'))");
    CHECK(parseWhere("['ANY AND EVERY', 'X', ['.', 'names'], ['=', ['?', 'X'], 'Smith']]")
          == "(fl_count(body, 'names') > 0 AND NOT EXISTS (SELECT 1 FROM fl_each(body, 'names') AS _X WHERE NOT (_X.value = 'Smith')))");
}


TEST_CASE("QueryParser ANY complex", "[Query]") {
    CHECK(parseWhere("['ANY', 'X', ['.', 'names'], ['=', ['?', 'X', 'last'], 'Smith']]")
          == "EXISTS (SELECT 1 FROM fl_each(body, 'names') AS _X WHERE fl_value(_X.pointer, 'last') = 'Smith')");
}


TEST_CASE("QueryParser SELECT", "[Query]") {
    CHECK(parseWhere("['SELECT', {WHAT: ['._id'],\
                                 WHERE: ['=', ['.', 'last'], 'Smith'],\
                              ORDER_BY: [['.', 'first'], ['.', 'age']]}]")
          == "SELECT key FROM kv_default WHERE fl_value(body, 'last') = 'Smith' ORDER BY fl_value(body, 'first'), fl_value(body, 'age')");
    CHECK(parseWhere("['array_count()', ['SELECT',\
                                  {WHAT: ['._id'],\
                                  WHERE: ['=', ['.', 'last'], 'Smith'],\
                               ORDER_BY: [['.', 'first'], ['.', 'age']]}]]")
          == "array_count(SELECT key FROM kv_default WHERE fl_value(body, 'last') = 'Smith' ORDER BY fl_value(body, 'first'), fl_value(body, 'age'))");
    // note this query is lowercase, to test case-insensitivity
    CHECK(parseWhere("['exists', ['select',\
                                  {what: ['._id'],\
                                  where: ['=', ['.', 'last'], 'Smith'],\
                               order_by: [['.', 'first'], ['.', 'age']]}]]")
          == "EXISTS (SELECT key FROM kv_default WHERE fl_value(body, 'last') = 'Smith' ORDER BY fl_value(body, 'first'), fl_value(body, 'age'))");
    CHECK(parseWhere("['EXISTS', ['SELECT',\
                                  {WHAT: [['MAX()', ['.weight']]],\
                                  WHERE: ['=', ['.', 'last'], 'Smith'],\
                               DISTINCT: true,\
                               GROUP_BY: [['.', 'first'], ['.', 'age']]}]]")
          == "EXISTS (SELECT DISTINCT max(fl_value(body, 'weight')) FROM kv_default WHERE fl_value(body, 'last') = 'Smith' GROUP BY fl_value(body, 'first'), fl_value(body, 'age'))");
}


TEST_CASE("QueryParser SELECT FTS", "[Query]") {
    CHECK(parseWhere("['SELECT', {\
                     WHERE: ['MATCH', ['.', 'bio'], 'mobile']}]")
          == "SELECT offsets(\"kv_default::.bio\") FROM kv_default JOIN \"kv_default::.bio\" AS FTS1 ON FTS1.rowid = kv_default.sequence WHERE FTS1.text MATCH 'mobile'");
}

TEST_CASE("QueryParser SELECT WHAT", "[Query]") {
    CHECK(parseWhere("['SELECT', {WHAT: ['._id'], WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT key FROM kv_default WHERE fl_value(body, 'last') = 'Smith'");
    CHECK(parseWhere("['SELECT', {WHAT: [['.first']],\
                                 WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT fl_value(body, 'first') FROM kv_default WHERE fl_value(body, 'last') = 'Smith'");
    CHECK(parseWhere("['SELECT', {WHAT: [['.first'], ['length()', ['.middle']]],\
                                 WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT fl_value(body, 'first'), length(fl_value(body, 'middle')) FROM kv_default WHERE fl_value(body, 'last') = 'Smith'");
}


TEST_CASE("QueryParser CASE", "[Query]") {
    CHECK(parseWhere("['CASE', ['.color'], 'red', 1, 'green', 2]")
          == "CASE fl_value(body, 'color') WHEN 'red' THEN 1 WHEN 'green' THEN 2 END");
    CHECK(parseWhere("['CASE', ['.color'], 'red', 1, 'green', 2, 0]")
          == "CASE fl_value(body, 'color') WHEN 'red' THEN 1 WHEN 'green' THEN 2 ELSE 0 END");
    CHECK(parseWhere("['CASE', null, ['=', 2, 3], 'wtf', ['=', 2, 2], 'right']")
          == "CASE WHEN 2 = 3 THEN 'wtf' WHEN 2 = 2 THEN 'right' END");
    CHECK(parseWhere("['CASE', null, ['=', 2, 3], 'wtf', ['=', 2, 2], 'right', 'whatever']")
          == "CASE WHEN 2 = 3 THEN 'wtf' WHEN 2 = 2 THEN 'right' ELSE 'whatever' END");
}


TEST_CASE("QueryParser Join", "[Query]") {
    CHECK(parse("{WHAT: ['.book.title', '.library.name'], \
                  FROM: [{as: 'book'}, \
                         {as: 'library', 'on': ['=', ['.book.library'], ['.library._id']]}],\
                 WHERE: ['=', ['.book.author'], ['$AUTHOR']]}")
          == "SELECT fl_value(\"book\".body, 'title'), fl_value(\"library\".body, 'name') FROM kv_default AS \"book\" JOIN kv_default AS \"library\" ON fl_value(\"book\".body, 'library') = \"library\".key WHERE fl_value(\"book\".body, 'author') = $_AUTHOR");
}


TEST_CASE("QueryParser Collate", "[Query][Collation]") {
    CHECK(parseWhere("['COLLATE', {unicode: true, locale:'se', case:false}, \
                                  ['=', ['.', 'name'], 'Puddin\\' Tane']]")
          == "(fl_value(body, 'name') = 'Puddin'' Tane') COLLATE LCUnicode_C__se");
    CHECK(parse("{WHAT: ['.book.title'], \
                  FROM: [{as: 'book'}],\
                 WHERE: ['=', ['.book.author'], ['$AUTHOR']], \
              ORDER_BY: [ ['COLLATE', {'unicode':true, 'case':false}, ['.book.title']] ]}")
          == "SELECT fl_value(\"book\".body, 'title') "
               "FROM kv_default AS \"book\" "
              "WHERE fl_value(\"book\".body, 'author') = $_AUTHOR "
           "ORDER BY fl_value(\"book\".body, 'title') COLLATE LCUnicode_C__");
}


TEST_CASE("QueryParser errors", "[Query][!throws]") {
    mustFail("['poop()', 1]");
    mustFail("['power()', 1]");
    mustFail("['power()', 1, 2, 3]");
    mustFail("['CASE', ['.color'], 'red']");
    mustFail("['CASE', null, 'red']");
}
