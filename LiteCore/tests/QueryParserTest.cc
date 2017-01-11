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


static string parseWhere(string json) {
    QueryParser qp("kv_default");
    alloc_slice fleece = JSONConverter::convertJSON(json5(json));
    qp.parseJustExpression(Value::fromTrustedData(fleece));
    return qp.SQL();
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
    CHECK(parseWhere("['=', ['pow()', 25, ['/', 1, 2]], 5]")
          == "pow(25, 1 / 2) = 5");
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
}


TEST_CASE("QueryParser bindings", "[Query]") {
    CHECK(parseWhere("['=', ['$', 'X'], ['$', 7]]")
          == "$_X = $_7");
    CHECK(parseWhere("['=', ['$X'], ['$', 7]]")
          == "$_X = $_7");
}


TEST_CASE("QueryParser special properties", "[Query]") {
    CHECK(parseWhere("['foo()', ['.', '_id'], ['.', '_sequence']]")
          == "foo(key, sequence)");
    CHECK(parseWhere("['foo()', ['._id'], ['.', '_sequence']]")
          == "foo(key, sequence)");
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


TEST_CASE("QueryParser SELECT", "[Query]") {
    CHECK(parseWhere("['SELECT', {/*WHAT: ['.', 'first'],*/\
                                  WHERE: ['=', ['.', 'last'], 'Smith'],\
                                 'ORDER BY': [['.', 'first'], ['.', 'age']]}]")
          == "SELECT * FROM kv_default WHERE fl_value(body, 'last') = 'Smith' ORDER BY fl_value(body, 'first'), fl_value(body, 'age')");
    CHECK(parseWhere("['array_count()', ['SELECT', {/*WHAT: ['.', 'first'],*/\
                                  WHERE: ['=', ['.', 'last'], 'Smith'],\
                                 'ORDER BY': [['.', 'first'], ['.', 'age']]}]]")
          == "array_count(SELECT * FROM kv_default WHERE fl_value(body, 'last') = 'Smith' ORDER BY fl_value(body, 'first'), fl_value(body, 'age'))");
    CHECK(parseWhere("['EXISTS', ['SELECT', {/*WHAT: ['.', 'first'],*/\
                                  WHERE: ['=', ['.', 'last'], 'Smith'],\
                                 'ORDER BY': [['.', 'first'], ['.', 'age']]}]]")
          == "EXISTS (SELECT * FROM kv_default WHERE fl_value(body, 'last') = 'Smith' ORDER BY fl_value(body, 'first'), fl_value(body, 'age'))");
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
