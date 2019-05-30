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

#include "QueryParserTest.hh"
#include "FleeceImpl.hh"
#include "Error.hh"
#include <vector>
#include <iostream>


string QueryParserTest::parse(FLValue val) {
    QueryParser qp(*this);
    qp.parse((const fleece::impl::Value*)val);
    return qp.SQL();
}

string QueryParserTest::parse(string json) {
    alloc_slice fleece = fleece::impl::JSONConverter::convertJSON(json5(json));
    return parse((FLValue)fleece::impl::Value::fromTrustedData(fleece));
}

string QueryParserTest::parseWhere(string json) {
    QueryParser qp(*this);
    alloc_slice fleece = fleece::impl::JSONConverter::convertJSON(json5(json));
    qp.parseJustExpression(fleece::impl::Value::fromTrustedData(fleece));
    return qp.SQL();
}

void QueryParserTest::mustFail(string json) {
    QueryParser qp(*this);
    alloc_slice fleece = fleece::impl::JSONConverter::convertJSON(json5(json));
    ExpectException(error::LiteCore, error::InvalidQuery, [&]{
        qp.parseJustExpression(fleece::impl::Value::fromTrustedData(fleece));
    });
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser basic", "[Query]") {
    CHECK(parseWhere("['=', ['.', 'name'], 'Puddin\\' Tane']")
          == "fl_value(body, 'name') = 'Puddin'' Tane'");
    CHECK(parseWhere("['=', ['.name'], 'Puddin\\' Tane']")
          == "fl_value(body, 'name') = 'Puddin'' Tane'");
    CHECK(parseWhere("['AND', ['=', ['.', 'again'], true], ['=', ['.', 'name'], 'Puddin\\' Tane']]")
          == "fl_value(body, 'again') = fl_bool(1) AND fl_value(body, 'name') = 'Puddin'' Tane'");
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
    CHECK(parseWhere("['=', ['.address'], {street:'123 Main St', city: ['.city']}]")
          == "fl_value(body, 'address') = dict_of('city', fl_value(body, 'city'), 'street', '123 Main St')");
    CHECK(parseWhere("['=', ['.address'], {}]")
          == "fl_value(body, 'address') = dict_of()");
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

    CHECK(parseWhere("['_.', ['.address'], 'zip']")
          == "fl_nested_value(fl_value(body, 'address'), 'zip')");
    CHECK(parseWhere("['_.zip', ['.address']]")
          == "fl_nested_value(fl_value(body, 'address'), 'zip')");
    CHECK(parseWhere("['_.', ['.addresses'], '[0]']")
          == "fl_nested_value(fl_value(body, 'addresses'), '[0]')");
    CHECK(parseWhere("['_.[0]', ['.addresses']]")
          == "fl_nested_value(fl_value(body, 'addresses'), '[0]')");
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


TEST_CASE_METHOD(QueryParserTest, "QueryParser Deletion", "[Query]") {
    CHECK(parseWhere("['SELECT', {WHAT: ['._id'], WHERE: ['._deleted']}]")
          == "SELECT fl_result(_doc.key) FROM kv_default AS _doc WHERE ((_doc.flags & 1 != 0))");
    CHECK(parseWhere("['SELECT', {WHAT: ['._id'], WHERE: ['OR', ['._deleted'], ['.junk']]}]")
          == "SELECT fl_result(_doc.key) FROM kv_default AS _doc WHERE ((_doc.flags & 1 != 0) OR fl_value(_doc.body, 'junk'))");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser Expiration", "[Query]") {
    CHECK(parseWhere("['SELECT', {WHAT: ['._id'], WHERE: ['IS NOT', ['._expiration'], ['MISSING']]}]")
          == "SELECT fl_result(_doc.key) FROM kv_default AS _doc WHERE (_doc.expiration IS NOT NULL) AND (_doc.flags & 1 = 0)");
    CHECK(parseWhere("['SELECT', {WHAT: ['._expiration'], WHERE: ['IS NOT', ['._expiration'], ['MISSING']]}]")
          == "SELECT fl_result(_doc.expiration) FROM kv_default AS _doc WHERE (_doc.expiration IS NOT NULL) AND (_doc.flags & 1 = 0)");
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
          == "SELECT \"person\".key, \"person\".sequence FROM kv_default AS \"person\" WHERE ((fl_contains(\"person\".body, 'names', 'Smith'))) AND (\"person\".flags & 1 = 0)");
    CHECK(parseWhere("['SELECT', {FROM: [{AS: 'person'}, {AS: 'book', 'ON': 1}],\
                                 WHERE: ['ANY', 'X', ['.', 'book', 'keywords'], ['=', ['?', 'X'], 'horror']]}]")
          == "SELECT \"person\".key, \"person\".sequence FROM kv_default AS \"person\" INNER JOIN kv_default AS \"book\" ON (1) AND (\"book\".flags & 1 = 0) WHERE ((fl_contains(\"book\".body, 'keywords', 'horror'))) AND (\"person\".flags & 1 = 0)");

    // Non-property calls:
    CHECK(parseWhere("['ANY', 'X', ['pi()'], ['=', ['?X'], 'Smith']]")
          == "fl_contains(pi(), null, 'Smith')");
    CHECK(parseWhere("['EVERY', 'X', ['pi()'], ['=', ['?', 'X'], 'Smith']]")
          == "NOT EXISTS (SELECT 1 FROM fl_each(pi()) AS _X WHERE NOT (_X.value = 'Smith'))");
    CHECK(parseWhere("['SELECT', {FROM: [{AS: 'person'}],\
                     WHERE: ['ANY', 'X', ['pi()'], ['=', ['?', 'X'], 'Smith']]}]")
          == "SELECT \"person\".key, \"person\".sequence FROM kv_default AS \"person\" WHERE ((fl_contains(pi(), null, 'Smith'))) AND (\"person\".flags & 1 = 0)");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser ANY complex", "[Query]") {
    CHECK(parseWhere("['ANY', 'X', ['.', 'names'], ['=', ['?', 'X', 'last'], 'Smith']]")
          == "EXISTS (SELECT 1 FROM fl_each(body, 'names') AS _X WHERE fl_nested_value(_X.body, 'last') = 'Smith')");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser SELECT", "[Query]") {
    CHECK(parseWhere("['SELECT', {WHAT: ['._id'],\
                                 WHERE: ['=', ['.', 'last'], 'Smith'],\
                              ORDER_BY: [['.', 'first'], ['.', 'age']]}]")
          == "SELECT fl_result(_doc.key) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1 = 0) ORDER BY fl_value(_doc.body, 'first'), fl_value(_doc.body, 'age')");
    CHECK(parseWhere("['array_count()', ['SELECT',\
                                  {WHAT: ['._id'],\
                                  WHERE: ['=', ['.', 'last'], 'Smith'],\
                               ORDER_BY: [['.', 'first'], ['.', 'age']]}]]")
          == "array_count(SELECT fl_result(_doc.key) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1 = 0) ORDER BY fl_value(_doc.body, 'first'), fl_value(_doc.body, 'age'))");
    // note this query is lowercase, to test case-insensitivity
    CHECK(parseWhere("['exists', ['select',\
                                  {what: ['._id'],\
                                  where: ['=', ['.', 'last'], 'Smith'],\
                               order_by: [['.', 'first'], ['.', 'age']]}]]")
          == "EXISTS (SELECT fl_result(_doc.key) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1 = 0) ORDER BY fl_value(_doc.body, 'first'), fl_value(_doc.body, 'age'))");
    CHECK(parseWhere("['EXISTS', ['SELECT',\
                                  {WHAT: [['MAX()', ['.weight']]],\
                                  WHERE: ['=', ['.', 'last'], 'Smith'],\
                               DISTINCT: true,\
                               GROUP_BY: [['.', 'first'], ['.', 'age']]}]]")
          == "EXISTS (SELECT DISTINCT fl_result(max(fl_value(_doc.body, 'weight'))) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1 = 0) GROUP BY fl_value(_doc.body, 'first'), fl_value(_doc.body, 'age'))");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser SELECT FTS", "[Query][FTS]") {
    CHECK(parseWhere("['SELECT', {\
                     WHERE: ['MATCH', 'bio', 'mobile']}]")
          == "SELECT _doc.rowid, offsets(fts1.\"kv_default::bio\"), key, sequence FROM kv_default AS _doc JOIN \"kv_default::bio\" AS fts1 ON fts1.docid = _doc.rowid WHERE (fts1.\"kv_default::bio\" MATCH 'mobile') AND (_doc.flags & 1 = 0)");
}


#if COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(QueryParserTest, "QueryParser SELECT prediction", "[Query][Predict]") {
    string pred = "['PREDICTION()', 'bias', {text: ['.text']}, '.bias']";
    auto query1 = "['SELECT', {WHERE: ['>', " + pred + ", 0] }]";
    auto query2 = "['SELECT', {WHERE: ['>', " + pred + ", 0], WHAT: [" + pred + "] }]";
    tablesExist = false;
    CHECK(parseWhere(query1)
          == "SELECT key, sequence FROM kv_default AS _doc WHERE (prediction('bias', dict_of('text', fl_value(_doc.body, 'text')), '.bias') > 0) AND (_doc.flags & 1 = 0)");
    CHECK(parseWhere(query2)
          == "SELECT fl_result(prediction('bias', dict_of('text', fl_value(_doc.body, 'text')), '.bias')) FROM kv_default AS _doc WHERE (prediction('bias', dict_of('text', fl_value(_doc.body, 'text')), '.bias') > 0) AND (_doc.flags & 1 = 0)");

    tablesExist = true;
    CHECK(parseWhere(query1)
          == "SELECT key, sequence FROM kv_default AS _doc JOIN \"kv_default:predict:dIrX6kaB9tP3x7oyJKq5st+23kE=\" AS pred1 ON pred1.docid = _doc.rowid WHERE (fl_unnested_value(pred1.body, 'bias') > 0) AND (_doc.flags & 1 = 0)");
    CHECK(parseWhere(query2)
          == "SELECT fl_result(fl_unnested_value(pred1.body, 'bias')) FROM kv_default AS _doc JOIN \"kv_default:predict:dIrX6kaB9tP3x7oyJKq5st+23kE=\" AS pred1 ON pred1.docid = _doc.rowid WHERE (fl_unnested_value(pred1.body, 'bias') > 0) AND (_doc.flags & 1 = 0)");
}
#endif


TEST_CASE_METHOD(QueryParserTest, "QueryParser SELECT WHAT", "[Query]") {
    CHECK(parseWhere("['SELECT', {WHAT: ['._id'], WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT fl_result(_doc.key) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1 = 0)");
    CHECK(parseWhere("['SELECT', {WHAT: [['.first']],\
                                 WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT fl_result(fl_value(_doc.body, 'first')) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1 = 0)");
    CHECK(parseWhere("['SELECT', {WHAT: [['.first'], ['length()', ['.middle']]],\
                                 WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT fl_result(fl_value(_doc.body, 'first')), fl_result(N1QL_length(fl_value(_doc.body, 'middle'))) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1 = 0)");
    CHECK(parseWhere("['SELECT', {WHAT: [['.first'], ['AS', ['length()', ['.middle']], 'mid']],\
                                 WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT fl_result(fl_value(_doc.body, 'first')), fl_result(N1QL_length(fl_value(_doc.body, 'middle'))) AS \"mid\" FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1 = 0)");
    // Check the "." operator (like SQL "*"):
    CHECK(parseWhere("['SELECT', {WHAT: ['.'], WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT fl_result(fl_root(_doc.body)) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1 = 0)");
    CHECK(parseWhere("['SELECT', {WHAT: [['.']], WHERE: ['=', ['.', 'last'], 'Smith']}]")
          == "SELECT fl_result(fl_root(_doc.body)) FROM kv_default AS _doc WHERE (fl_value(_doc.body, 'last') = 'Smith') AND (_doc.flags & 1 = 0)");
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
          == "SELECT fl_result(fl_value(\"book\".body, 'title')), fl_result(fl_value(\"library\".body, 'name')), fl_result(fl_root(\"library\".body)) FROM kv_default AS \"book\" INNER JOIN kv_default AS \"library\" ON (fl_value(\"book\".body, 'library') = \"library\".key) AND (\"library\".flags & 1 = 0) WHERE (fl_value(\"book\".body, 'author') = $_AUTHOR) AND (\"book\".flags & 1 = 0)");

    // Multiple JOINs (#363):
    CHECK(parse("{'WHAT':[['.','session','appId'],['.','user','username'],['.','session','emoId']],\
                  'FROM': [{'as':'session'},\
                           {'as':'user','on':['=',['.','session','emoId'],['.','user','emoId']]},\
                           {'as':'licence','on':['=',['.','session','licenceID'],['.','licence','id']]}],\
                 'WHERE':['AND',['AND',['=',['.','session','type'],'session'],['=',['.','user','type'],'user']],['=',['.','licence','type'],'licence']]}")
          == "SELECT fl_result(fl_value(\"session\".body, 'appId')), fl_result(fl_value(\"user\".body, 'username')), fl_result(fl_value(\"session\".body, 'emoId')) FROM kv_default AS \"session\" INNER JOIN kv_default AS \"user\" ON (fl_value(\"session\".body, 'emoId') = fl_value(\"user\".body, 'emoId')) AND (\"user\".flags & 1 = 0) INNER JOIN kv_default AS \"licence\" ON (fl_value(\"session\".body, 'licenceID') = fl_value(\"licence\".body, 'id')) AND (\"licence\".flags & 1 = 0) WHERE ((fl_value(\"session\".body, 'type') = 'session' AND fl_value(\"user\".body, 'type') = 'user') AND fl_value(\"licence\".body, 'type') = 'licence') AND (\"session\".flags & 1 = 0)");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser SELECT UNNEST", "[Query][FTS]") {
    CHECK(parseWhere("['SELECT', {\
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['.book.notes']}],\
                     WHERE: ['=', ['.notes'], 'torn']}]")
          == "SELECT \"book\".key, \"book\".sequence FROM kv_default AS \"book\" JOIN fl_each(\"book\".body, 'notes') AS \"notes\" WHERE (\"notes\".value = 'torn') AND (\"book\".flags & 1 = 0)");
    CHECK(parseWhere("['SELECT', {\
                      WHAT: ['.notes'], \
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['.book.notes']}],\
                     WHERE: ['>', ['.notes.page'], 100]}]")
          == "SELECT fl_result(\"notes\".value) FROM kv_default AS \"book\" JOIN fl_each(\"book\".body, 'notes') AS \"notes\" WHERE (fl_nested_value(\"notes\".body, 'page') > 100) AND (\"book\".flags & 1 = 0)");
    CHECK(parseWhere("['SELECT', {\
                      WHAT: ['.notes'], \
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['pi()']}],\
                     WHERE: ['>', ['.notes.page'], 100]}]")
          == "SELECT fl_result(\"notes\".value) FROM kv_default AS \"book\" JOIN fl_each(pi()) AS \"notes\" WHERE (fl_nested_value(\"notes\".body, 'page') > 100) AND (\"book\".flags & 1 = 0)");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser SELECT UNNEST optimized", "[Query][FTS]") {
    tablesExist = true;
    
    CHECK(parseWhere("['SELECT', {\
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['.book.notes']}],\
                     WHERE: ['=', ['.notes'], 'torn']}]")
          == "SELECT \"book\".key, \"book\".sequence FROM kv_default AS \"book\" JOIN \"kv_default:unnest:notes\" AS \"notes\" ON \"notes\".docid=\"book\".rowid WHERE (fl_unnested_value(\"notes\".body) = 'torn') AND (\"book\".flags & 1 = 0)");
    CHECK(parseWhere("['SELECT', {\
                      WHAT: ['.notes'], \
                      FROM: [{as: 'book'}, \
                             {as: 'notes', 'unnest': ['.book.notes']}],\
                     WHERE: ['>', ['.notes.page'], 100]}]")
          == "SELECT fl_result(fl_unnested_value(\"notes\".body)) FROM kv_default AS \"book\" JOIN \"kv_default:unnest:notes\" AS \"notes\" ON \"notes\".docid=\"book\".rowid WHERE (fl_unnested_value(\"notes\".body, 'page') > 100) AND (\"book\".flags & 1 = 0)");
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
              "WHERE (fl_value(\"book\".body, 'author') = $_AUTHOR) AND (\"book\".flags & 1 = 0) "
           "ORDER BY fl_value(\"book\".body, 'title') COLLATE \"LCUnicode_C__\"");
}


TEST_CASE_METHOD(QueryParserTest, "QueryParser errors", "[Query][!throws]") {
    mustFail("['poop()', 1]");
    mustFail("['power()', 1]");
    mustFail("['power()', 1, 2, 3]");
    mustFail("['CASE', ['.color'], 'red']");
    mustFail("['CASE', null, 'red']");
}
