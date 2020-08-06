//
// c4N1QLParserTest.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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
#include "n1ql_parser.hh"
#include "StringUtil.hh"
#include "fleece/Mutable.hh"
#include <iostream>


using namespace std;
using namespace litecore;
using namespace fleece;

class N1QLParserTest : QueryParserTest {
protected:

    string translate(const char *n1ql) {
        cerr << n1ql << "\n-->  " ;
        C4Error error;
        unsigned errorPos;

        FLValue dict = (FLValue) n1ql::parse(n1ql, &errorPos);
        if (!dict) {
            cerr << string(max(errorPos,5u)-5, ' ') << "^--syntax error\n";
            return "";
        }

        string jsonResult = string(alloc_slice(FLValue_ToJSONX((FLValue)dict, false, true)));
        replace(jsonResult, '"', '\'');     // save the tests from having to escape tons of quotes
        cerr << jsonResult << "\n";

        string sql = parse(dict);
        cerr << "-->  " << sql << "\n";
        
        FLValue_Release(dict);
        return jsonResult;
    }
};

// NOTE: the translate() method converts `"` to `'` in its output, to make the string literals
// in the tests below less cumbersome to type (and read).

TEST_CASE_METHOD(N1QLParserTest, "N1QL literals", "[Query][N1QL][C]") {
    CHECK(translate("SELECT FALSE") == "{'WHAT':[false]}");
    CHECK(translate("SELECT TRUE") == "{'WHAT':[true]}");
    CHECK(translate("SELECT NULL") == "{'WHAT':[null]}");
    CHECK(translate("SELECT MISSING") == "{'WHAT':[['MISSING']]}");

    CHECK(translate("SELECT 0") == "{'WHAT':[0]}");
    CHECK(translate("SELECT 17") == "{'WHAT':[17]}");
    CHECK(translate("SELECT -17") == "{'WHAT':[-17]}");
    CHECK(translate("SELECT 17.25") == "{'WHAT':[17.25]}");
    CHECK(translate("SELECT -17.25") == "{'WHAT':[-17.25]}");
    CHECK(translate("SELECT 17.25e2") == "{'WHAT':[1725.0]}");
    CHECK(translate("SELECT 17.25E+02") == "{'WHAT':[1725.0]}");
    CHECK(translate("SELECT 17.25e02") == "{'WHAT':[1725.0]}");
    CHECK(translate("SELECT 1625e-02") == "{'WHAT':[16.25]}");
    CHECK(translate("SELECT .25") == "{'WHAT':[0.25]}");

    CHECK(translate("SELECT []") == "{'WHAT':[['[]']]}");
    CHECK(translate("SELECT [17]") == "{'WHAT':[['[]',17]]}");
    CHECK(translate("SELECT [  17  ] ") == "{'WHAT':[['[]',17]]}");
    CHECK(translate("SELECT [17,null, [], 'hi'||'there']") == "{'WHAT':[['[]',17,null,['[]'],['||','hi','there']]]}");

    CHECK(translate("SELECT ['hi']") == "{'WHAT':[['[]','hi']]}");
    CHECK(translate("SELECT ['foo bar']") == "{'WHAT':[['[]','foo bar']]}");
    CHECK(translate("SELECT ['foo ''or'' bar']") == "{'WHAT':[['[]','foo 'or' bar']]}");

    CHECK(translate("SELECT [\"hi\"]") == "{'WHAT':[['[]','hi']]}");
    CHECK(translate("SELECT [\"foo bar\"]") == "{'WHAT':[['[]','foo bar']]}");
    CHECK(translate("SELECT [\"foo \"\"or\"\" bar\"]") == "{'WHAT':[['[]','foo \\'or\\' bar']]}");

    CHECK(translate("SELECT {}") == "{'WHAT':[{}]}");
    CHECK(translate("SELECT {'x':17}") == "{'WHAT':[{'x':17}]}");
    CHECK(translate("SELECT { 'x' :  17  } ") == "{'WHAT':[{'x':17}]}");
    CHECK(translate("SELECT {'x':17, 'null': null,'empty':{} , 'str':'hi'||'there'}") == "{'WHAT':[{'empty':{},'null':null,'str':['||','hi','there'],'x':17}]}");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL properties", "[Query][N1QL][C]") {
    CHECK(translate("select foo") == "{'WHAT':[['.foo']]}");
    CHECK(translate("select foo9$_X") == "{'WHAT':[['.foo9\\\\$_X']]}");
    CHECK(translate("select foo.bar") == "{'WHAT':[['.foo.bar']]}");
    CHECK(translate("select foo. bar . baz") == "{'WHAT':[['.foo.bar.baz']]}");

    CHECK(translate("select `foo bar`") == "{'WHAT':[['.foo bar']]}");
    CHECK(translate("select `foo ``bar``baz`") == "{'WHAT':[['.foo `bar`baz']]}");

    CHECK(translate("select `mr.grieves`.`hey`") == "{'WHAT':[['.mr\\\\.grieves.hey']]}");
    CHECK(translate("select `$type`") == "{'WHAT':[['.\\\\$type']]}");

    CHECK(translate("select meta.id") == "{'WHAT':[['._id']]}");
    CHECK(translate("select meta.sequence") == "{'WHAT':[['._sequence']]}");
    CHECK(translate("select meta.deleted") == "{'WHAT':[['._deleted']]}");
    CHECK(translate("select db.meta.id") == "{'WHAT':[['.db._id']]}");
    CHECK(translate("select meta.bogus") == "");    // only specific meta properties allowed
    CHECK(translate("select db.meta.bogus") == "");

    CHECK(translate("select foo[17]") == "{'WHAT':[['.foo[17]']]}");
    CHECK(translate("select foo.bar[-1].baz") == "{'WHAT':[['.foo.bar[-1].baz']]}");

    CHECK(translate("SELECT *") == "{'WHAT':[['.']]}");
    CHECK(translate("SELECT db.*") == "{'WHAT':[['.db.']]}");

    CHECK(translate("select $var") == "{'WHAT':[['$var']]}");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL expressions", "[Query][N1QL][C]") {
    CHECK(translate("SELECT -x") == "{'WHAT':[['-',['.x']]]}");
    CHECK(translate("SELECT NOT x") == "{'WHAT':[['NOT',['.x']]]}");

    CHECK(translate("SELECT 17+0") == "{'WHAT':[['+',17,0]]}");
    CHECK(translate("SELECT 17 + 0") == "{'WHAT':[['+',17,0]]}");
    CHECK(translate("SELECT 17 > 0") == "{'WHAT':[['>',17,0]]}");
    CHECK(translate("SELECT 17='hi'") == "{'WHAT':[['=',17,'hi']]}");
    CHECK(translate("SELECT 17 = 'hi'") == "{'WHAT':[['=',17,'hi']]}");
    CHECK(translate("SELECT 17 == 'hi'") == "{'WHAT':[['=',17,'hi']]}");
    CHECK(translate("SELECT 17 != 'hi'") == "{'WHAT':[['!=',17,'hi']]}");
    CHECK(translate("SELECT 17 <>'hi'") == "{'WHAT':[['!=',17,'hi']]}");

    CHECK(translate("SELECT 3+4) from x") == "");

    CHECK(translate("SELECT 17 IN (1, 2, 3)") == "{'WHAT':[['IN',17,['[]',1,2,3]]]}");
    CHECK(translate("SELECT 17 NOT IN (1, 2, 3)") == "{'WHAT':[['NOT IN',17,['[]',1,2,3]]]}");

    CHECK(translate("SELECT 6 IS 9") == "{'WHAT':[['IS',6,9]]}");
    CHECK(translate("SELECT 6 IS NOT 9") == "{'WHAT':[['IS NOT',6,9]]}");
    CHECK(translate("SELECT 6 NOT NULL") == "{'WHAT':[['IS NOT',6,null]]}");

    CHECK(translate("SELECT 'foo' LIKE 'f%'") == "{'WHAT':[['LIKE','foo','f%']]}");
    CHECK(translate("SELECT 'foo' NOT LIKE 'f%'") == "{'WHAT':[['NOT',['LIKE','foo','f%']]]}");
    CHECK(translate("SELECT 1 WHERE 'text' MATCH 'word'") == "{'WHAT':[1],'WHERE':['MATCH','text','word']}");
//    CHECK(translate("SELECT 1 WHERE 'text' NOT MATCH 'word'") == "{'WHAT':[['NOT',['MATCH',['.text'],'word']]]}");

    CHECK(translate("SELECT 2 BETWEEN 1 AND 4") == "{'WHAT':[['BETWEEN',2,1,4]]}");
    CHECK(translate("SELECT 2 NOT BETWEEN 1 AND 4") == "{'WHAT':[['NOT',['BETWEEN',2,1,4]]]}");
    CHECK(translate("SELECT 2+3 BETWEEN 1+1 AND 4+4") == "{'WHAT':[['BETWEEN',['+',2,3],['+',1,1],['+',4,4]]]}");

    // Check for left-associativity and correct operator precedence:
    CHECK(translate("SELECT 3 + 4 + 5 + 6") == "{'WHAT':[['+',['+',['+',3,4],5],6]]}");
    CHECK(translate("SELECT 3 - 4 - 5 - 6") == "{'WHAT':[['-',['-',['-',3,4],5],6]]}");
    CHECK(translate("SELECT 3 + 4 * 5 - 6") == "{'WHAT':[['-',['+',3,['*',4,5]],6]]}");

    CHECK(translate("SELECT (3 + 4) * (5 - 6)") == "{'WHAT':[['*',['+',3,4],['-',5,6]]]}");

    CHECK(translate("SELECT type='airline' and callsign not null") == "{'WHAT':[['AND',['=',['.type'],'airline'],['IS NOT',['.callsign'],null]]]}");

    CHECK(translate("SELECT * WHERE ANY x IN addresses SATISFIES x.zip = 94040 OR x = 0 OR xy = x END") ==
          "{'WHAT':[['.']],'WHERE':['ANY','x',['.addresses'],['OR',['OR',['=',['?x.zip'],94040],"
          "['=',['?x'],0]],['=',['.xy'],['?x']]]]}");
    CHECK(translate("SELECT * WHERE ANY AND EVERY x IN addresses SATISFIES x.zip = 94040 OR x = 0 OR xy = x END") ==
          "{'WHAT':[['.']],'WHERE':['ANY AND EVERY','x',['.addresses'],['OR',['OR',['=',['?x.zip'],94040],"
          "['=',['?x'],0]],['=',['.xy'],['?x']]]]}");
    CHECK(translate("SELECT * WHERE SOME x IN addresses SATISFIES x.zip = 94040 OR x = 0 OR xy = x END") ==
          "{'WHAT':[['.']],'WHERE':['ANY','x',['.addresses'],['OR',['OR',['=',['?x.zip'],94040],"
          "['=',['?x'],0]],['=',['.xy'],['?x']]]]}");
    CHECK(translate("SELECT ANY review IN reviewList SATISFIES review='review2042' END AND NOT (unitPrice<10)") == "{'WHAT':[['AND',['ANY','review',['.reviewList'],['=',['?review'],'review2042']],['NOT',['<',['.unitPrice'],10]]]]}");

    CHECK(translate("SELECT CASE x WHEN 1 THEN 'one' END") == "{'WHAT':[['CASE',['.x'],1,'one']]}");
    CHECK(translate("SELECT CASE x WHEN 1 THEN 'one' WHEN 2 THEN 'two' END") == "{'WHAT':[['CASE',['.x'],1,'one',2,'two']]}");
    CHECK(translate("SELECT CASE x WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'duhh' END") == "{'WHAT':[['CASE',['.x'],1,'one',2,'two','duhh']]}");
    CHECK(translate("SELECT CASE WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'duhh' END") == "{'WHAT':[['CASE',null,1,'one',2,'two','duhh']]}");

    CHECK(translate("SELECT {'x':17}.x") == "{'WHAT':[['_.',{'x':17},'.x']]}");
    CHECK(translate("SELECT {'x':17}.xx.yy") == "{'WHAT':[['_.',{'x':17},'.xx.yy']]}");
    CHECK(translate("SELECT {'x':17}.xx[0].yy") == "{'WHAT':[['_.',{'x':17},'.xx[0].yy']]}");

    CHECK(translate("SELECT EXISTS (SELECT 6 IS 9)") == "{'WHAT':[['EXISTS',['SELECT',{'WHAT':[['IS',6,9]]}]]]}");

}

TEST_CASE_METHOD(N1QLParserTest, "N1QL functions", "[Query][N1QL][C]") {
    CHECK(translate("SELECT squee()") == "");   // unknown name

    CHECK(translate("SELECT pi()") == "{'WHAT':[['pi()']]}");
    CHECK(translate("SELECT sin(1)") == "{'WHAT':[['sin()',1]]}");
    CHECK(translate("SELECT power(1, 2)") == "{'WHAT':[['power()',1,2]]}");
    CHECK(translate("SELECT power(1, cos(2))") == "{'WHAT':[['power()',1,['cos()',2]]]}");

    CHECK(translate("SELECT count(*)") == "{'WHAT':[['count()',['.']]]}");
    CHECK(translate("SELECT count(db.*)") == "{'WHAT':[['count()',['.db.']]]}");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL collation", "[Query][N1QL][C]") {
    CHECK(translate("SELECT (name = 'fred') COLLATE NOCASE") == "{'WHAT':[['COLLATE',{'CASE':false},['=',['.name'],'fred']]]}");
    CHECK(translate("SELECT (name = 'fred') COLLATE UNICODE CASE NODIACRITICS") == "{'WHAT':[['COLLATE',{'CASE':true,'DIACRITICS':false,'UNICODE':true},['=',['.name'],'fred']]]}");
    CHECK(translate("SELECT (name = 'fred') COLLATE NOCASE FRED") == "");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL SELECT", "[Query][N1QL][C]") {
    CHECK(translate("SELECT foo") == "{'WHAT':[['.foo']]}");
    CHECK(translate("SELECT ALL foo") == "{'WHAT':[['.foo']]}");
    CHECK(translate("SELECT DISTINCT foo") == "{'DISTINCT':true,'WHAT':[['.foo']]}");

    CHECK(translate("SELECT foo bar") == "");
    CHECK(translate("SELECT from where true") == "");
    CHECK(translate("SELECT `from` where true") == "{'WHAT':[['.from']],'WHERE':true}");

    CHECK(translate("SELECT foo, bar") == "{'WHAT':[['.foo'],['.bar']]}");
    CHECK(translate("SELECT foo as A, bar as B") == "{'WHAT':[['AS',['.foo'],'A'],['AS',['.bar'],'B']]}");

    CHECK(translate("SELECT foo WHERE 10") == "{'WHAT':[['.foo']],'WHERE':10}");
    CHECK(translate("SELECT WHERE 10") == "");
    CHECK(translate("SELECT foo WHERE foo = 'hi'") == "{'WHAT':[['.foo']],'WHERE':['=',['.foo'],'hi']}");

    CHECK(translate("SELECT foo GROUP BY bar") == "{'GROUP_BY':[['.bar']],'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo GROUP BY bar, baz") == "{'GROUP_BY':[['.bar'],['.baz']],'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo GROUP BY bar, baz HAVING hi") == "{'GROUP_BY':[['.bar'],['.baz']],'HAVING':['.hi'],'WHAT':[['.foo']]}");

    CHECK(translate("SELECT foo ORDER BY bar") == "{'ORDER_BY':[['.bar']],'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo ORDER BY bar ASC") == "{'ORDER_BY':[['ASC',['.bar']]],'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo ORDER BY bar DESC") == "{'ORDER_BY':[['DESC',['.bar']]],'WHAT':[['.foo']]}");

    CHECK(translate("SELECT foo LIMIT 10") == "{'LIMIT':10,'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo OFFSET 20") == "{'OFFSET':20,'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo LIMIT 10 OFFSET 20") == "{'LIMIT':10,'OFFSET':20,'WHAT':[['.foo']]}");

// QueryParser does not support "IN SELECT" yet
//    CHECK(translate("SELECT 17 NOT IN (SELECT value WHERE type='prime')") == "{'WHAT':[['NOT IN',17,['SELECT',{'WHAT':[['.value']],'WHERE':['=',['.type'],'prime']}]]]}");

    CHECK(translate("SELECT productId, color, categories WHERE categories[0] LIKE 'Bed%' AND test_id='where_func' ORDER BY productId LIMIT 3") == "{'LIMIT':3,'ORDER_BY':[['.productId']],'WHAT':[['.productId'],['.color'],['.categories']],'WHERE':['AND',['LIKE',['.categories[0]'],'Bed%'],['=',['.test_id'],'where_func']]}");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL JOIN", "[Query][N1QL][C]") {
    CHECK(translate("SELECT 0 FROM db") == "{'FROM':[{'AS':'db'}],'WHAT':[0]}");
    CHECK(translate("SELECT * FROM db") == "{'FROM':[{'AS':'db'}],'WHAT':[['.']]}");
    CHECK(translate("SELECT file.name FROM db AS file") == "{'FROM':[{'AS':'file'}],'WHAT':[['.file.name']]}");
    CHECK(translate("SELECT file.name FROM db file") ==                    // omit 'AS'
          "{'FROM':[{'AS':'file'}],'WHAT':[['.file.name']]}");
    CHECK(translate("SELECT db.name FROM db JOIN db AS other ON other.key = db.key")
          == "{'FROM':[{'AS':'db'},{'AS':'other','JOIN':'INNER','ON':['=',['.other.key'],['.db.key']]}],'WHAT':[['.db.name']]}");
    CHECK(translate("SELECT db.name FROM db JOIN db other ON other.key = db.key") // omit 'AS'
          == "{'FROM':[{'AS':'db'},{'AS':'other','JOIN':'INNER','ON':['=',['.other.key'],['.db.key']]}],'WHAT':[['.db.name']]}");
    CHECK(translate("SELECT db.name FROM db JOIN db AS other ON other.key = db.key CROSS JOIN x")
          == "{'FROM':[{'AS':'db'},{'AS':'other','JOIN':'INNER','ON':['=',['.other.key'],['.db.key']]},{'AS':'x','JOIN':'CROSS'}],'WHAT':[['.db.name']]}");
}
