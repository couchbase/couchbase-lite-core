//
// QueryParserTest.cc
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

#include "QueryParser.hh"
#include "Error.hh"
#include "FleeceImpl.hh"
#include <string>
#include <vector>
#include <iostream>
#include "LiteCoreTest.hh"

using namespace std;
using namespace fleece::impl;


class QueryParserTest : public QueryParser::delegate {
public:
    QueryParserTest() { }

    virtual std::string tableName() const override {
        return "kv_default";
    }
    virtual std::string FTSTableName(const std::string &property) const override {
        return tableName() + "::" + property;
    }
    virtual std::string unnestedTableName(const std::string &property) const override {
        return tableName() + ":unnest:" + property;
    }
    virtual bool tableExists(const string &tableName) const override {
        return tablesExist;
    }

protected:
    string parse(string json) {
        QueryParser qp(*this);
        alloc_slice fleece = JSONConverter::convertJSON(json5(json));
        qp.parse(Value::fromTrustedData(fleece));
        return qp.SQL();
    }

    string parseWhere(string json) {
        QueryParser qp(*this);
        alloc_slice fleece = JSONConverter::convertJSON(json5(json));
        qp.parseJustExpression(Value::fromTrustedData(fleece));
        return qp.SQL();
    }

    void mustFail(string json) {
        QueryParser qp(*this);
        alloc_slice fleece = JSONConverter::convertJSON(json5(json));
        ExpectException(error::LiteCore, error::InvalidQuery, [&]{
            qp.parseJustExpression(Value::fromTrustedData(fleece));
        });
    }

    bool tablesExist {false};

};


TEST_CASE_METHOD(QueryParserTest, "QueryParser basic", "[Query]") {
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
    CHECK(parseWhere("['=', ['.', 'candies'], ['[]', 'm&ms', 'jujubes']]")
          == "fl_value(body, 'candies') = array_of('m&ms', 'jujubes')");
    CHECK(parseWhere("['IN', ['.', 'name'], ['[]', 'Webbis', 'Wowbagger']]")
          == "fl_value(body, 'name') IN ('Webbis', 'Wowbagger')");
    CHECK(parseWhere("['NOT IN', ['.', 'name'], ['[]', 'Webbis', 'Wowbagger']]")
          == "fl_value(body, 'name') NOT IN ('Webbis', 'Wowbagger')");
    CHECK(parseWhere("['IN', 'licorice', ['.', 'candies']]")
          == "array_contains(fl_value(body, 'candies'), 'licorice')");
    CHECK(parseWhere("['NOT IN', 7, ['.', 'ages']]")
          == "(NOT array_contains(fl_value(body, 'ages'), 7))");
    CHECK(parseWhere("['.', 'addresses', [1], 'zip']")
          == "fl_value(body, 'addresses[1].zip')");
    CHECK(parseWhere("['.', 'addresses', [1], 'zip']")
          == "fl_value(body, 'addresses[1].zip')");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser bindings", "[Query]") {
    CHECK(parseWhere("['=', ['$', 'X'], ['$', 7]]")
          == "$_X = $_7");
    CHECK(parseWhere("['=', ['$X'], ['$', 7]]")
          == "$_X = $_7");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser special properties", "[Query]") {
    CHECK(parseWhere("['ifnull()', ['.', '_id'], ['.', '_sequence']]")
          == "N1QL_ifnull(key, sequence)");
    CHECK(parseWhere("['ifnull()', ['._id'], ['.', '_sequence']]")
          == "N1QL_ifnull(key, sequence)");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser property contexts", "[Query]") {
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


TEST_CASE_METHOD(QueryParserTest, "QueryParser ANY", "[Query]") {
    CHECK(parseWhere("['ANY', 'X', ['.', 'names'], ['=', ['?', 'X'], 'Smith']]")
          == "fl_contains(body, 'names', 'Smith')");
    CHECK(parseWhere("['ANY', 'X', ['.', 'names'], ['=', ['?X'], 'Smith']]")
          == "fl_contains(body, 'names', 'Smith')");
    CHECK(parseWhere("['ANY', 'X', ['.', 'names'], ['>', ['?', 'X'], 3.125]]")
          == "EXISTS (SELECT 1 FROM fl_each(body, 'names') AS _X WHERE _X.value > 3.125)");
    CHECK(parseWhere("['EVERY', 'X', ['.', 'names'], ['=', ['?', 'X'], 'Smith']]")
          == "NOT EXISTS (SELECT 1 FROM fl_each(body, 'names') AS _X WHERE NOT (_X.value = 'Smith'))");
    CHECK(parseWhere("['ANY AND EVERY', 'X', ['.', 'names'], ['=', ['?', 'X'], 'Smith']]")
          == "(fl_count(body, 'names') > 0 AND NOT EXISTS (SELECT 1 FROM fl_each(body, 'names') AS _X WHERE NOT (_X.value = 'Smith')))");

    CHECK(parseWhere("['SELECT', {FROM: [{AS: 'person'}],\
                                 WHERE: ['ANY', 'X', ['.', 'person', 'names'], ['=', ['?', 'X'], 'Smith']]}]")
          == "SELECT \"person\".key, \"person\".sequence FROM kv_default AS \"person\" WHERE ((fl_contains(\"person\".body, 'names', 'Smith'))) AND (\"person\".flags & 1) = 0");
    CHECK(parseWhere("['SELECT', {FROM: [{AS: 'person'}, {AS: 'book', 'ON': 1}],\
                                 WHERE: ['ANY', 'X', ['.', 'book', 'keywords'], ['=', ['?', 'X'], 'horror']]}]")
          == "SELECT \"person\".key, \"person\".sequence FROM kv_default AS \"person\" CROSS JOIN kv_default AS \"book\" ON (1) AND (\"book\".flags & 1) = 0 WHERE ((fl_contains(\"book\".body, 'keywords', 'horror'))) AND (\"person\".flags & 1) = 0");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser ANY complex", "[Query]") {
    CHECK(parseWhere("['ANY', 'X', ['.', 'names'], ['=', ['?', 'X', 'last'], 'Smith']]")
          == "EXISTS (SELECT 1 FROM fl_each(body, 'names') AS _X WHERE fl_nested_value(_X.body, 'last') = 'Smith')");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser SELECT", "[Query]") {
    CHECK(parseWhere("['SELECT', {WHAT: ['._id'],\
                                 WHERE: ['=', ['.', 'last'], 'Smith'],\
                              ORDER_BY: [['.', 'first'], ['.', 'age']]}]")
          == "SELECT fl_result(_doc.key) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1) = 0 ORDER BY fl_value(_doc.body, 'first'), fl_value(_doc.body, 'age')");
    CHECK(parseWhere("['array_count()', ['SELECT',\
                                  {WHAT: ['._id'],\
                                  WHERE: ['=', ['.', 'last'], 'Smith'],\
                               ORDER_BY: [['.', 'first'], ['.', 'age']]}]]")
          == "array_count(SELECT fl_result(_doc.key) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1) = 0 ORDER BY fl_value(_doc.body, 'first'), fl_value(_doc.body, 'age'))");
    // note this query is lowercase, to test case-insensitivity
    CHECK(parseWhere("['exists', ['select',\
                                  {what: ['._id'],\
                                  where: ['=', ['.', 'last'], 'Smith'],\
                               order_by: [['.', 'first'], ['.', 'age']]}]]")
          == "EXISTS (SELECT fl_result(_doc.key) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1) = 0 ORDER BY fl_value(_doc.body, 'first'), fl_value(_doc.body, 'age'))");
    CHECK(parseWhere("['EXISTS', ['SELECT',\
                                  {WHAT: [['MAX()', ['.weight']]],\
                                  WHERE: ['=', ['.', 'last'], 'Smith'],\
                               DISTINCT: true,\
                               GROUP_BY: [['.', 'first'], ['.', 'age']]}]]")
          == "EXISTS (SELECT DISTINCT fl_result(max(fl_value(_doc.body, 'weight'))) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1) = 0 GROUP BY fl_value(_doc.body, 'first'), fl_value(_doc.body, 'age'))");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser SELECT FTS", "[Query][FTS]") {
    CHECK(parseWhere("['SELECT', {\
                     WHERE: ['MATCH', 'bio', 'mobile']}]")
          == "SELECT _doc.rowid, offsets(\"kv_default::bio\"), key, sequence FROM kv_default AS _doc JOIN \"kv_default::bio\" AS FTS1 ON FTS1.docid = _doc.rowid WHERE (FTS1.\"kv_default::bio\" MATCH 'mobile') AND (_doc.flags & 1) = 0");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser SELECT WHAT", "[Query]") {
    CHECK(parseWhere("['SELECT', {WHAT: ['._id'], WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT fl_result(_doc.key) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1) = 0");
    CHECK(parseWhere("['SELECT', {WHAT: [['.first']],\
                                 WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT fl_result(fl_value(_doc.body, 'first')) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1) = 0");
    CHECK(parseWhere("['SELECT', {WHAT: [['.first'], ['length()', ['.middle']]],\
                                 WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT fl_result(fl_value(_doc.body, 'first')), fl_result(N1QL_length(fl_value(_doc.body, 'middle'))) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1) = 0");
    // Check the "." operator (like SQL "*"):
    CHECK(parseWhere("['SELECT', {WHAT: ['.'], WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT fl_result(fl_root(_doc.body)) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1) = 0");
    CHECK(parseWhere("['SELECT', {WHAT: [['.']], WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT fl_result(fl_root(_doc.body)) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1) = 0");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser CASE", "[Query]") {
    CHECK(parseWhere("['CASE', ['.color'], 'red', 1, 'green', 2]")
          == "CASE fl_value(body, 'color') WHEN 'red' THEN 1 WHEN 'green' THEN 2 END");
    CHECK(parseWhere("['CASE', ['.color'], 'red', 1, 'green', 2, 0]")
          == "CASE fl_value(body, 'color') WHEN 'red' THEN 1 WHEN 'green' THEN 2 ELSE 0 END");
    CHECK(parseWhere("['CASE', null, ['=', 2, 3], 'wtf', ['=', 2, 2], 'right']")
          == "CASE WHEN 2 = 3 THEN 'wtf' WHEN 2 = 2 THEN 'right' END");
    CHECK(parseWhere("['CASE', null, ['=', 2, 3], 'wtf', ['=', 2, 2], 'right', 'whatever']")
          == "CASE WHEN 2 = 3 THEN 'wtf' WHEN 2 = 2 THEN 'right' ELSE 'whatever' END");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser Join", "[Query]") {
    CHECK(parse("{WHAT: ['.book.title', '.library.name', '.library'], \
                  FROM: [{as: 'book'}, \
                         {as: 'library', 'on': ['=', ['.book.library'], ['.library._id']]}],\
                 WHERE: ['=', ['.book.author'], ['$AUTHOR']]}")
          == "SELECT fl_result(fl_value(\"book\".body, 'title')), fl_result(fl_value(\"library\".body, 'name')), fl_result(fl_root(\"library\".body)) FROM kv_default AS \"book\" CROSS JOIN kv_default AS \"library\" ON (fl_value(\"book\".body, 'library') = \"library\".key) AND (\"library\".flags & 1) = 0 WHERE (fl_value(\"book\".body, 'author') = $_AUTHOR) AND (\"book\".flags & 1) = 0");

    // Multiple JOINs (#363):
    CHECK(parse("{'WHAT':[['.','session','appId'],['.','user','username'],['.','session','emoId']],\
                  'FROM': [{'as':'session'},\
                           {'as':'user','on':['=',['.','session','emoId'],['.','user','emoId']]},\
                           {'as':'licence','on':['=',['.','session','licenceID'],['.','licence','id']]}],\
                 'WHERE':['AND',['AND',['=',['.','session','type'],'session'],['=',['.','user','type'],'user']],['=',['.','licence','type'],'licence']]}")
          == "SELECT fl_result(fl_value(\"session\".body, 'appId')), fl_result(fl_value(\"user\".body, 'username')), fl_result(fl_value(\"session\".body, 'emoId')) FROM kv_default AS \"session\" CROSS JOIN kv_default AS \"user\" ON (fl_value(\"session\".body, 'emoId') = fl_value(\"user\".body, 'emoId')) AND (\"user\".flags & 1) = 0 CROSS JOIN kv_default AS \"licence\" ON (fl_value(\"session\".body, 'licenceID') = fl_value(\"licence\".body, 'id')) AND (\"licence\".flags & 1) = 0 WHERE ((fl_value(\"session\".body, 'type') = 'session' AND fl_value(\"user\".body, 'type') = 'user') AND fl_value(\"licence\".body, 'type') = 'licence') AND (\"session\".flags & 1) = 0");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser SELECT UNNEST", "[Query][FTS]") {
    CHECK(parseWhere("['SELECT', {\
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['.book.notes']}],\
                     WHERE: ['=', ['.notes'], 'torn']}]")
          == "SELECT \"book\".key, \"book\".sequence FROM kv_default AS \"book\" JOIN fl_each(\"book\".body, 'notes') AS \"notes\" WHERE (\"notes\".value = 'torn') AND (\"book\".flags & 1) = 0");
    CHECK(parseWhere("['SELECT', {\
                      WHAT: ['.notes'], \
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['.book.notes']}],\
                     WHERE: ['>', ['.notes.page'], 100]}]")
          == "SELECT fl_result(\"notes\".value) FROM kv_default AS \"book\" JOIN fl_each(\"book\".body, 'notes') AS \"notes\" WHERE (fl_nested_value(\"notes\".body, 'page') > 100) AND (\"book\".flags & 1) = 0");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser SELECT UNNEST optimized", "[Query][FTS]") {
    tablesExist = true;
    
    CHECK(parseWhere("['SELECT', {\
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['.book.notes']}],\
                     WHERE: ['=', ['.notes'], 'torn']}]")
          == "SELECT \"book\".key, \"book\".sequence FROM kv_default AS \"book\" JOIN \"kv_default:unnest:notes\" AS \"notes\" ON \"notes\".docid=\"book\".rowid WHERE (fl_unnested_value(\"notes\".body) = 'torn') AND (\"book\".flags & 1) = 0");
    CHECK(parseWhere("['SELECT', {\
                      WHAT: ['.notes'], \
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['.book.notes']}],\
                     WHERE: ['>', ['.notes.page'], 100]}]")
          == "SELECT fl_result(fl_unnested_value(\"notes\".body)) FROM kv_default AS \"book\" JOIN \"kv_default:unnest:notes\" AS \"notes\" ON \"notes\".docid=\"book\".rowid WHERE (fl_unnested_value(\"notes\".body, 'page') > 100) AND (\"book\".flags & 1) = 0");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser Collate", "[Query][Collation]") {
    CHECK(parseWhere("['AND',['COLLATE',{'UNICODE':true,'CASE':false,'DIAC':false},['=',['.Artist'],['$ARTIST']]],['IS',['.Compilation'],['MISSING']]]")
          == "fl_value(body, 'Artist') COLLATE \"LCUnicode_CD_\" = $_ARTIST AND fl_value(body, 'Compilation') IS NULL");
    CHECK(parseWhere("['COLLATE', {unicode: true, locale:'se', case:false}, \
                                  ['=', ['.', 'name'], 'Puddin\\' Tane']]")
          == "fl_value(body, 'name') COLLATE \"LCUnicode_C__se\" = 'Puddin'' Tane'");
    CHECK(parseWhere("['COLLATE', {unicode: true, locale:'yue_Hans_CN', case:false}, \
                     ['=', ['.', 'name'], 'Puddin\\' Tane']]")
          == "fl_value(body, 'name') COLLATE \"LCUnicode_C__yue_Hans_CN\" = 'Puddin'' Tane'");
    CHECK(parse("{WHAT: ['.book.title'], \
                  FROM: [{as: 'book'}],\
                 WHERE: ['=', ['.book.author'], ['$AUTHOR']], \
              ORDER_BY: [ ['COLLATE', {'unicode':true, 'case':false}, ['.book.title']] ]}")
          == "SELECT fl_result(fl_value(\"book\".body, 'title')) "
               "FROM kv_default AS \"book\" "
              "WHERE (fl_value(\"book\".body, 'author') = $_AUTHOR) AND (\"book\".flags & 1) = 0 "
           "ORDER BY fl_value(\"book\".body, 'title') COLLATE \"LCUnicode_C__\"");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser errors", "[Query][!throws]") {
    mustFail("['poop()', 1]");
    mustFail("['power()', 1]");
    mustFail("['power()', 1, 2, 3]");
    mustFail("['CASE', ['.color'], 'red']");
    mustFail("['CASE', null, 'red']");
}
