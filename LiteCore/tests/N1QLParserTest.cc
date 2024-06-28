//
// c4N1QLParserTest.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "QueryParserTest.hh"
#include "catch.hpp"
#include "n1ql_parser.hh"
#include "Stopwatch.hh"
#include "StringUtil.hh"
#include <iostream>


using namespace std;
using namespace litecore;
using namespace fleece;

class N1QLParserTest : public QueryParserTest {
  protected:
    // Translates N1QL to JSON, with strings single-quoted to avoid tons of escapes in the tests.
    // On syntax error, returns "".
    string translate(const char* n1ql) {
        UNSCOPED_INFO("N1QL: " << n1ql);
        int errorPos;

        FLValue dict = (FLValue)n1ql::parse(n1ql, &errorPos);
        if ( !dict ) {
            UNSCOPED_INFO(string(errorPos + 6, ' ') << "^--syntax error");
            return "";
        }

        string jsonResult = string(alloc_slice(FLValue_ToJSONX((FLValue)dict, false, true)));
        replace(jsonResult, '"', '\'');
        UNSCOPED_INFO(jsonResult);

        string sql = parse(dict);
        UNSCOPED_INFO("-->  " << sql);

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
    CHECK(translate("SELECT 9223372036854775807") == "{'WHAT':[9223372036854775807]}");
    CHECK(translate("SELECT -9223372036854775808") == "{'WHAT':[-9223372036854775808]}");

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
    CHECK(translate("SELECT {'x':17, 'null': null,'empty':{} , 'str':'hi'||'there'}")
          == "{'WHAT':[{'empty':{},'null':null,'str':['||','hi','there'],'x':17}]}");
    string withNewline = R"r(
        SELECT *
        FROM _
    )r";
    CHECK(translate(withNewline.c_str()) == "{'FROM':[{'COLLECTION':'_'}],'WHAT':[['.']]}");
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

    CHECK(translate("select meta().id") == "{'WHAT':[['_.',['meta()'],'.id']]}");
    CHECK(translate("select meta(id).id from _default as id")
          == "{'FROM':[{'AS':'id','COLLECTION':'_default'}],"
             "'WHAT':[['_.',['meta()','id'],'.id']]}");
    CHECK(translate("select meta().sequence") == "{'WHAT':[['_.',['meta()'],'.sequence']]}");
    CHECK(translate("select meta().revisionID") == "{'WHAT':[['_.',['meta()'],'.revisionID']]}");
    CHECK(translate("select meta().deleted") == "{'WHAT':[['_.',['meta()'],'.deleted']]}");
    CHECK(translate("select meta(_default).id from _default")
          == "{'FROM':[{'COLLECTION':'_default'}],'WHAT':[['_.',['meta()','_default'],'.id']]}");
    {
        ExpectingExceptions x;
        CHECK_THROWS_WITH(translate("select meta().bogus"), "'bogus' is not a valid Meta key");
        CHECK_THROWS_WITH(translate("select meta(_default).bogus from _default"), "'bogus' is not a valid Meta key");
        CHECK_THROWS_WITH(translate("select meta(id).id as id"),
                          "database alias 'id' does not match a declared 'AS' alias");
    }
    CHECK(translate("select foo[17]") == "{'WHAT':[['.foo[17]']]}");
    CHECK(translate("select foo.bar[-1].baz") == "{'WHAT':[['.foo.bar[-1].baz']]}");

    CHECK(translate("SELECT *") == "{'WHAT':[['.']]}");
    // The following query is correct in grammar, but it actully will return the "db" property of the
    // default collection.
    CHECK(translate("SELECT db.*") == "{'WHAT':[['.db.']]}");
    CHECK(translate("SELECT db.* FROM _ db") == "{'FROM':[{'AS':'db','COLLECTION':'_'}],'WHAT':[['.db.']]}");
    // The database alias is back-quoted
    CHECK(translate("SELECT `db.c`.* FROM _ AS `db.c`")
          == "{'FROM':[{'AS':'db\\\\.c','COLLECTION':'_'}],'WHAT':[['.db\\\\.c.']]}");

    CHECK(translate("select $var") == "{'WHAT':[['$var']]}");

    // "custId" is implicitly scoped by the unique alias, "orders".
    CHECK(translate("SELECT DISTINCT custId FROM _default AS orders where test_id = 'agg_func' ORDER BY custId")
          == "{'DISTINCT':true,'FROM':[{'AS':'orders','COLLECTION':'_default'}],'ORDER_BY':[['.custId']],"
             "'WHAT':[['.custId']],'WHERE':['=',['.test_id'],'agg_func']}");
    {
        ExpectingExceptions x;
        CHECK_THROWS_WITH(translate("SELECT custId, other.custId FROM _default AS orders JOIN _default as other "
                                    "ON orders.test_id = other.test_id ORDER BY custId"),
                          "property 'custId' does not begin with a declared 'AS' alias");
    }

    // Quoting special chars in properties
    CHECK(translate("select `string[0]`") == "{'WHAT':[['.string\\\\[0]']]}");
    CHECK(translate("select `string[0]`.arr[2].`string[3]`.simpleID")
          == "{'WHAT':[['.string\\\\[0].arr[2].string\\\\[3].simpleID']]}");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL expressions", "[Query][N1QL][C]") {
    tableNames.insert("stuff");

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

    CHECK(translate("SELECT 3+4) from stuff").empty());

    CHECK(translate("SELECT 17 IN (1, 2, 3)") == "{'WHAT':[['IN',17,['[]',1,2,3]]]}");
    CHECK(translate("SELECT 17 NOT IN (1, 2, 3)") == "{'WHAT':[['NOT IN',17,['[]',1,2,3]]]}");

    CHECK(translate("SELECT 17 IN [1, 2, 3]") == "{'WHAT':[['IN',17,['[]',1,2,3]]]}");
    CHECK(translate("SELECT 17 NOT IN [1, 2, 3]") == "{'WHAT':[['NOT IN',17,['[]',1,2,3]]]}");

    CHECK(translate("SELECT 6 IS 9") == "{'WHAT':[['IS',6,9]]}");
    CHECK(translate("SELECT 6 IS NOT 9") == "{'WHAT':[['IS NOT',6,9]]}");
    CHECK(translate("SELECT 6 NOT NULL") == "{'WHAT':[['IS NOT',6,null]]}");
    CHECK(translate("SELECT 6 WHERE x IS   NOT   VALUED") == "{'WHAT':[6],'WHERE':['NOT',['IS VALUED',['.x']]]}");
    CHECK(translate("SELECT 6 WHERE x  IS  VALUED") == "{'WHAT':[6],'WHERE':['IS VALUED',['.x']]}");

    CHECK(translate("SELECT 'foo' LIKE 'f%'") == "{'WHAT':[['LIKE','foo','f%']]}");
    CHECK(translate("SELECT 'foo' NOT LIKE 'f%'") == "{'WHAT':[['NOT',['LIKE','foo','f%']]]}");
    CHECK(translate("SELECT 1 WHERE MATCH(text, 'word') ORDER BY RANK(text)")
          == "{'ORDER_BY':[['RANK()','text']],'WHAT':[1],'WHERE':['MATCH()','text','word']}");
    CHECK(translate("SELECT 1 WHERE MATCH(`text`, 'word')") == "{'WHAT':[1],'WHERE':['MATCH()','text','word']}");
    CHECK(translate("SELECT 1 WHERE MATCH('text', 'word')")
                  .empty());  // The first argument to MATCH must be an identifier.
    CHECK(translate("SELECT 1 WHERE MATCH(text, 'word') ORDER BY RANK('text')")
                  .empty());  // The argument to RANK must be an identifier.
    //    CHECK(translate("SELECT 1 WHERE 'text' NOT MATCH 'word'") == "{'WHAT':[['NOT',['MATCH',['.text'],'word']]]}");

    CHECK(translate("SELECT 2 BETWEEN 1 AND 4") == "{'WHAT':[['BETWEEN',2,1,4]]}");
    CHECK(translate("SELECT 2 NOT BETWEEN 1 AND 4") == "{'WHAT':[['NOT',['BETWEEN',2,1,4]]]}");
    CHECK(translate("SELECT 2+3 BETWEEN 1+1 AND 4+4") == "{'WHAT':[['BETWEEN',['+',2,3],['+',1,1],['+',4,4]]]}");

    // Check for left-associativity and correct operator precedence:
    CHECK(translate("SELECT 3 + 4 + 5 + 6") == "{'WHAT':[['+',['+',['+',3,4],5],6]]}");
    CHECK(translate("SELECT 3 - 4 - 5 - 6") == "{'WHAT':[['-',['-',['-',3,4],5],6]]}");
    CHECK(translate("SELECT 3 + 4 * 5 - 6") == "{'WHAT':[['-',['+',3,['*',4,5]],6]]}");

    CHECK(translate("SELECT (3 + 4) * (5 - 6)") == "{'WHAT':[['*',['+',3,4],['-',5,6]]]}");

    CHECK(translate("SELECT type='airline' and callsign not null")
          == "{'WHAT':[['AND',['=',['.type'],'airline'],['IS NOT',['.callsign'],null]]]}");

    CHECK(translate("SELECT * WHERE ANY x IN addresses SATISFIES x.zip = 94040 OR x = 0 OR xy = x END")
          == "{'WHAT':[['.']],'WHERE':['ANY','x',['.addresses'],['OR',['OR',['=',['?x.zip'],94040],"
             "['=',['?x'],0]],['=',['.xy'],['?x']]]]}");
    CHECK(translate("SELECT * WHERE ANY AND EVERY x IN addresses SATISFIES x.zip = 94040 OR x = 0 OR xy = x END")
          == "{'WHAT':[['.']],'WHERE':['ANY AND EVERY','x',['.addresses'],['OR',['OR',['=',['?x.zip'],94040],"
             "['=',['?x'],0]],['=',['.xy'],['?x']]]]}");
    CHECK(translate("SELECT * WHERE SOME x IN addresses SATISFIES x.zip = 94040 OR x = 0 OR xy = x END")
          == "{'WHAT':[['.']],'WHERE':['ANY','x',['.addresses'],['OR',['OR',['=',['?x.zip'],94040],"
             "['=',['?x'],0]],['=',['.xy'],['?x']]]]}");
    CHECK(translate("SELECT ANY review IN reviewList SATISFIES review='review2042' END AND NOT (unitPrice<10)")
          == "{'WHAT':[['AND',['ANY','review',['.reviewList'],['=',['?review'],'review2042']],['NOT',['<',['.unitPrice'"
             "],10]]]]}");

    CHECK(translate("SELECT CASE x WHEN 1 THEN 'one' END") == "{'WHAT':[['CASE',['.x'],1,'one']]}");
    CHECK(translate("SELECT CASE x WHEN 1 THEN 'one' WHEN 2 THEN 'two' END")
          == "{'WHAT':[['CASE',['.x'],1,'one',2,'two']]}");
    CHECK(translate("SELECT CASE x WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'duhh' END")
          == "{'WHAT':[['CASE',['.x'],1,'one',2,'two','duhh']]}");
    CHECK(translate("SELECT CASE WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'duhh' END")
          == "{'WHAT':[['CASE',null,1,'one',2,'two','duhh']]}");

    CHECK(translate("SELECT {'x':17}.x") == "{'WHAT':[['_.',{'x':17},'.x']]}");
    CHECK(translate("SELECT {'x':17}.xx.yy") == "{'WHAT':[['_.',{'x':17},'.xx.yy']]}");
    CHECK(translate("SELECT {'x':17}.xx[0].yy") == "{'WHAT':[['_.',{'x':17},'.xx[0].yy']]}");

    CHECK(translate("SELECT EXISTS (SELECT 6 IS 9)") == "{'WHAT':[['EXISTS',['SELECT',{'WHAT':[['IS',6,9]]}]]]}");

    CHECK(translate("SELECT product.categories CATG, COUNT(*) AS numprods WHERE test_id = \"agg_func\" "
                    "GROUP BY product.categories HAVING COUNT(*) BETWEEN 15 and 30 ORDER BY CATG, numprods LIMIT 3")
          == "{'GROUP_BY':[['.product.categories']],"
             "'HAVING':['BETWEEN',['COUNT()',['.']],15,30],"
             "'LIMIT':3,"
             "'ORDER_BY':[['.CATG'],['.numprods']],"
             "'WHAT':[['AS',['.product.categories'],'CATG'],['AS',['COUNT()',['.']],'numprods']],"
             "'WHERE':['=',['.test_id'],'agg_func']}");
    CHECK(translate("SELECT product.categories CATG, COUNT ( * ) AS numprods WHERE test_id = \"agg_func\" "
                    "GROUP BY product.categories HAVING COUNT(*) BETWEEN POWER ( ABS(-2) , ABS(3) ) and 30 ORDER BY "
                    "CATG, numprods LIMIT 3")
          == "{'GROUP_BY':[['.product.categories']],"
             "'HAVING':['BETWEEN',['COUNT()',['.']],['POWER()',['ABS()',-2],['ABS()',3]],30],"
             "'LIMIT':3,"
             "'ORDER_BY':[['.CATG'],['.numprods']],"
             "'WHAT':[['AS',['.product.categories'],'CATG'],['AS',['COUNT()',['.']],'numprods']],"
             "'WHERE':['=',['.test_id'],'agg_func']}");
    CHECK(translate("SELECT unitPrice, name FROM _default AS product WHERE unitPrice IS NOT MISSING AND "
                    "test_id=\"where_func\" ORDER BY unitPrice, productId LIMIT 3")
          == "{'FROM':[{'AS':'product','COLLECTION':'_default'}],'LIMIT':3,'ORDER_BY':[['.unitPrice'],['.productId']],"
             "'WHAT':[['.unitPrice'],['.name']],"
             "'WHERE':['AND',['IS NOT',['.unitPrice'],['MISSING']],['=',['.test_id'],'where_func']]}");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL functions", "[Query][N1QL][C]") {
    CHECK(translate("SELECT squee()").empty());  // unknown name

    CHECK(translate("SELECT pi()") == "{'WHAT':[['pi()']]}");
    CHECK(translate("SELECT sin(1)") == "{'WHAT':[['sin()',1]]}");
    CHECK(translate("SELECT power(1, 2)") == "{'WHAT':[['power()',1,2]]}");
    CHECK(translate("SELECT power(1, cos(2))") == "{'WHAT':[['power()',1,['cos()',2]]]}");

    CHECK(translate("SELECT count(*)") == "{'WHAT':[['count()',['.']]]}");
    CHECK(translate("SELECT count(db.*)") == "{'WHAT':[['count()',['.db.']]]}");
    CHECK(translate("SELECT concat(a, b)") == "{'WHAT':[['concat()',['.a'],['.b']]]}");
    CHECK(translate("SELECT concat('hello', \"world\", ' ', concat(true, 123.45 , sin(1)))")
          == "{'WHAT':[['concat()','hello','world',' ',['concat()',true,123.45,['sin()',1]]]]}");
#ifdef COUCHBASE_ENTERPRISE
    CHECK(translate("SELECT PREDICTION(factors, {\"numbers\" : num}, vec)")
          == "{'WHAT':[['PREDICTION()','factors',{'numbers':['.num']},['.vec']]]}");
    CHECK(translate("SELECT PREDICTION(factors, {\"numbers\" : num})")
          == "{'WHAT':[['PREDICTION()','factors',{'numbers':['.num']}]]}");
#endif
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL collation", "[Query][N1QL][C]") {
    CHECK(translate("SELECT (name = 'fred') COLLATE NOCASE")
          == "{'WHAT':[['COLLATE',{'CASE':false},['=',['.name'],'fred']]]}");
    CHECK(translate("SELECT (name = 'fred') COLLATE (UNICODE CASE NODIAC)")
          == "{'WHAT':[['COLLATE',{'CASE':true,'DIAC':false,'UNICODE':true},['=',['.name'],'fred']]]}");
    CHECK(translate("SELECT (name = 'fred') COLLATE UNICODE NOCASE").empty());
    CHECK(translate("SELECT (name = 'fred') COLLATE (NOCASE FRED)").empty());
    CHECK(translate("SELECT (name = 'fred') COLLATE NOCASE FRED")
          == "{'WHAT':[['AS',['COLLATE',{'CASE':false},['=',['.name'],'fred']],'FRED']]}");
    CHECK(translate("SELECT (name = 'fred') COLLATE (NOCASE) FRED")
          == "{'WHAT':[['AS',['COLLATE',{'CASE':false},['=',['.name'],'fred']],'FRED']]}");
    CHECK(translate("SELECT (name = 'fred') COLLATE UNICODE:se")
          == "{'WHAT':[['COLLATE',{'LOCALE':'se','UNICODE':true},['=',['.name'],'fred']]]}");
    CHECK(translate("SELECT (name = 'fred') COLLATE NOUNICODE")
          == "{'WHAT':[['COLLATE',{'UNICODE':false},['=',['.name'],'fred']]]}");
    CHECK(translate("SELECT (name = 'fred') COLLATE (NOUNICODE:se NOCASE DIAC)").empty());
    CHECK(translate("SELECT (name = 'fred') COLLATE (NOCASE unicode:se DIAC)")
          == "{'WHAT':[['COLLATE',{'CASE':false,'DIAC':true,'LOCALE':'se','UNICODE':true}"
             ",['=',['.name'],'fred']]]}");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL SELECT", "[Query][N1QL][C]") {
    CHECK(translate("SELECT foo") == "{'WHAT':[['.foo']]}");
    CHECK(translate("SELECT ALL foo") == "{'WHAT':[['.foo']]}");
    CHECK(translate("SELECT DISTINCT foo") == "{'DISTINCT':true,'WHAT':[['.foo']]}");

    CHECK(translate("SELECT foo bar") == "{'WHAT':[['AS',['.foo'],'bar']]}");
    CHECK(translate("SELECT from where true").empty());
    CHECK(translate("SELECT `from` where true") == "{'WHAT':[['.from']],'WHERE':true}");

    CHECK(translate("SELECT foo, bar") == "{'WHAT':[['.foo'],['.bar']]}");
    CHECK(translate("SELECT foo as A, bar as B") == "{'WHAT':[['AS',['.foo'],'A'],['AS',['.bar'],'B']]}");

    CHECK(translate("SELECT foo WHERE 10") == "{'WHAT':[['.foo']],'WHERE':10}");
    CHECK(translate("SELECT WHERE 10").empty());
    CHECK(translate("SELECT foo WHERE foo = 'hi'") == "{'WHAT':[['.foo']],'WHERE':['=',['.foo'],'hi']}");

    CHECK(translate("SELECT foo GROUP BY bar") == "{'GROUP_BY':[['.bar']],'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo GROUP BY bar, baz") == "{'GROUP_BY':[['.bar'],['.baz']],'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo GROUP BY bar, baz HAVING hi")
          == "{'GROUP_BY':[['.bar'],['.baz']],'HAVING':['.hi'],'WHAT':[['.foo']]}");

    CHECK(translate("SELECT foo ORDER BY bar") == "{'ORDER_BY':[['.bar']],'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo ORDER BY bar ASC") == "{'ORDER_BY':[['ASC',['.bar']]],'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo ORDER BY bar DESC") == "{'ORDER_BY':[['DESC',['.bar']]],'WHAT':[['.foo']]}");

    CHECK(translate("SELECT foo LIMIT 10") == "{'LIMIT':10,'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo OFFSET 20") == "{'OFFSET':20,'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo LIMIT 10 OFFSET 20") == "{'LIMIT':10,'OFFSET':20,'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo OFFSET 20 LIMIT 10") == "{'LIMIT':10,'OFFSET':20,'WHAT':[['.foo']]}");
    CHECK(translate("SELECT orderlines[0] WHERE test_id='order_func' ORDER BY orderlines[0].productId, "
                    "orderlines[0].qty ASC OFFSET 8192 LIMIT 1")
          == "{'LIMIT':1,'OFFSET':8192,'ORDER_BY':[['.orderlines[0].productId'],"
             "['ASC',['.orderlines[0].qty']]],'WHAT':[['.orderlines[0]']],'WHERE':['=',['.test_id'],'order_func']}");

    CHECK(translate("SELECT foo FROM _") == "{'FROM':[{'COLLECTION':'_'}],'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo FROM _default") == "{'FROM':[{'COLLECTION':'_default'}],'WHAT':[['.foo']]}");

    // QueryParser does not support "IN SELECT" yet
    //    CHECK(translate("SELECT 17 NOT IN (SELECT value WHERE type='prime')") == "{'WHAT':[['NOT IN',17,['SELECT',{'WHAT':[['.value']],'WHERE':['=',['.type'],'prime']}]]]}");

    tableNames.insert("kv_.product");

    CHECK(translate("SELECT productId, color, categories WHERE categories[0] LIKE 'Bed%' AND test_id='where_func' "
                    "ORDER BY productId LIMIT 3")
          == "{'LIMIT':3,'ORDER_BY':[['.productId']],'WHAT':[['.productId'],['.color'],['.categories']],'WHERE':['AND',"
             "['LIKE',['.categories[0]'],'Bed%'],['=',['.test_id'],'where_func']]}");
    CHECK(translate("SELECT FLOOR(unitPrice+0.5) as sc FROM product where test_id = \"numberfunc\" ORDER BY sc limit 5")
          == "{'FROM':[{'COLLECTION':'product'}],'LIMIT':5,'ORDER_BY':[['.sc']],"
             "'WHAT':[['AS',['FLOOR()',['+',['.unitPrice'],0.5]],'sc']],'WHERE':['=',['.test_id'],'numberfunc']}");

    CHECK(translate("SELECT META().id AS id WHERE META().id = $ID")
          == "{'WHAT':[['AS',['_.',['meta()'],'.id'],'id']],'WHERE':['=',['_.',['meta()'],'.id'],['$ID']]}");
    CHECK(translate("SELECT META().id AS id WHERE id = $ID")
          == "{'WHAT':[['AS',['_.',['meta()'],'.id'],'id']],'WHERE':['=',['.id'],['$ID']]}");

    tableNames.insert("kv_.store.customers");
    tableNames.insert("kv_.store2.customers");

    CHECK(translate("SELECT name FROM store.customers")
          == "{'FROM':[{'COLLECTION':'customers','SCOPE':'store'}],'WHAT':[['.name']]}");
    CHECK(translate("SELECT customers.name FROM store.customers")
          == "{'FROM':[{'COLLECTION':'customers','SCOPE':'store'}],'WHAT':[['.customers.name']]}");
    CHECK(translate("SELECT store.customers.name FROM store.customers")
          == "{'FROM':[{'COLLECTION':'customers','SCOPE':'store'}],'WHAT':[['.store.customers.name']]}");
    CHECK(translate("SELECT store.customers.name, store2.customers.name FROM store.customers"
                    " JOIN store2.customers ON store.customers.name = store2.customers.name")
          == "{'FROM':[{'COLLECTION':'customers','SCOPE':'store'},"
             "{'COLLECTION':'customers','JOIN':'INNER',"
             "'ON':['=',['.store.customers.name'],['.store2.customers.name']],'SCOPE':'store2'}],"
             "'WHAT':[['.store.customers.name'],['.store2.customers.name']]}");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL JOIN", "[Query][N1QL][C]") {
    tableNames.insert("kv_.db");
    tableNames.insert("kv_.other");
    tableNames.insert("kv_.x");

    CHECK(translate("SELECT 0 FROM db") == "{'FROM':[{'COLLECTION':'db'}],'WHAT':[0]}");
    CHECK(translate("SELECT * FROM db") == "{'FROM':[{'COLLECTION':'db'}],'WHAT':[['.']]}");
    CHECK(translate("SELECT file.name FROM db AS file")
          == "{'FROM':[{'AS':'file','COLLECTION':'db'}],'WHAT':[['.file.name']]}");
    CHECK(translate("SELECT file.name FROM db file")  // omit 'AS'
          == "{'FROM':[{'AS':'file','COLLECTION':'db'}],"
             "'WHAT':[['.file.name']]}");
    CHECK(translate("SELECT db.name FROM db JOIN other ON other.key = db.key")
          == "{'FROM':[{'COLLECTION':'db'},"
             "{'COLLECTION':'other','JOIN':'INNER','ON':['=',['.other.key'],['.db.key']]}],"
             "'WHAT':[['.db.name']]}");
    CHECK(translate("SELECT db.name FROM db JOIN x other ON other.key = db.key")  // omit 'AS'
          == "{'FROM':[{'COLLECTION':'db'},"
             "{'AS':'other','COLLECTION':'x','JOIN':'INNER','ON':['=',['.other.key'],['.db.key']]}],"
             "'WHAT':[['.db.name']]}");
    CHECK(translate("SELECT db.name FROM db JOIN other ON other.key = db.key CROSS JOIN x")
          == "{'FROM':[{'COLLECTION':'db'},"
             "{'COLLECTION':'other','JOIN':'INNER','ON':['=',['.other.key'],['.db.key']]},"
             "{'COLLECTION':'x','JOIN':'CROSS'}],"
             "'WHAT':[['.db.name']]}");
    CHECK(translate("SELECT rec, dss, dem FROM db rec LEFT JOIN db dss ON rec.sessionId = meta(dss).id "
                    "LEFT JOIN db dem ON rec.demId = meta(dem).id WHERE meta(rec).id LIKE 'rec:%'")
          == "{'FROM':[{'AS':'rec','COLLECTION':'db'},"
             "{'AS':'dss','COLLECTION':'db','JOIN':'LEFT','ON':['=',['.rec.sessionId'],['_.',['meta()','dss'],'.id']]},"
             "{'AS':'dem','COLLECTION':'db','JOIN':'LEFT','ON':['=',['.rec.demId'],['_.',['meta()','dem'],'.id']]}],"
             "'WHAT':[['.rec'],['.dss'],['.dem']],"
             "'WHERE':['LIKE',['_.',['meta()','rec'],'.id'],'rec:%']}");
    CHECK(translate("SELECT a, b, c FROM db a JOIN other b ON (a.n = b.n) JOIN x c ON (b.m = c.m) WHERE a.type = "
                    "b.type AND b.type = c.type")
          == "{'FROM':[{'AS':'a','COLLECTION':'db'},"
             "{'AS':'b','COLLECTION':'other','JOIN':'INNER','ON':['=',['.a.n'],['.b.n']]},"
             "{'AS':'c','COLLECTION':'x','JOIN':'INNER','ON':['=',['.b.m'],['.c.m']]}],"
             "'WHAT':[['.a'],['.b'],['.c']],"
             "'WHERE':['AND',['=',['.a.type'],['.b.type']],['=',['.b.type'],['.c.type']]]}");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL type-checking/conversion functions", "[Query][N1QL][C]") {
    CHECK(translate("SELECT isarray(x),  isatom(x),  isboolean(x),  isnumber(x),  isobject(x),  isstring(x),  type(x)")
          == "{'WHAT':[['isarray()',['.x']],['isatom()',['.x']],['isboolean()',['.x']],['isnumber()',['.x']],"
             "['isobject()',['.x']],['isstring()',['.x']],['type()',['.x']]]}");
    CHECK(translate("SELECT is_array(x),  is_atom(x),  is_boolean(x),  is_number(x),  is_object(x),  is_string(x),  "
                    "typename(x)")
          == "{'WHAT':[['is_array()',['.x']],['is_atom()',['.x']],['is_boolean()',['.x']],['is_number()',['.x']],"
             "['is_object()',['.x']],['is_string()',['.x']],['typename()',['.x']]]}");
    CHECK(translate("SELECT toarray(x),  toatom(x),  toboolean(x),  tonumber(x),  toobject(x),  tostring(x)")
          == "{'WHAT':[['toarray()',['.x']],['toatom()',['.x']],['toboolean()',['.x']],['tonumber()',['.x']],"
             "['toobject()',['.x']],['tostring()',['.x']]]}");
    CHECK(translate("SELECT to_array(x),  to_atom(x),  to_boolean(x),  to_number(x),  to_object(x),  to_string(x)")
          == "{'WHAT':[['to_array()',['.x']],['to_atom()',['.x']],['to_boolean()',['.x']],['to_number()',['.x']],"
             "['to_object()',['.x']],['to_string()',['.x']]]}");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL Scopes and Collections", "[Query][N1QL][C]") {
    tableNames.emplace("kv_.coll");
    tableNames.emplace("kv_.scope.coll");

    CHECK(translate("SELECT x FROM coll ORDER BY y")
          == "{'FROM':[{'COLLECTION':'coll'}],'ORDER_BY':[['.y']],'WHAT':[['.x']]}");
    CHECK(translate("SELECT x FROM scope.coll ORDER BY y")
          == "{'FROM':[{'COLLECTION':'coll','SCOPE':'scope'}],'ORDER_BY':[['.y']],'WHAT':[['.x']]}");
    CHECK(translate("SELECT coll.x, scoped.y FROM coll CROSS JOIN scope.coll scoped")
          == "{'FROM':[{'COLLECTION':'coll'},{'AS':'scoped','COLLECTION':'coll','JOIN':'CROSS','SCOPE':'scope'}],"
             "'WHAT':[['.coll.x'],['.scoped.y']]}");
    CHECK(translate("SELECT a.x, b.y FROM coll a JOIN scope.coll b ON a.name = b.name")
          == "{'FROM':[{'AS':'a','COLLECTION':'coll'},"
             "{'AS':'b','COLLECTION':'coll','JOIN':'INNER','ON':['=',['.a.name'],['.b.name']],'SCOPE':'scope'}],"
             "'WHAT':[['.a.x'],['.b.y']]}");
    CHECK(translate("SELECT a.x FROM coll a JOIN scope.coll b ON a.name = b.name "
                    "WHERE MATCH(a.ftsIndex, b.y)")
          == "{'FROM':[{'AS':'a','COLLECTION':'coll'},{'AS':'b','COLLECTION':'coll','JOIN':'INNER',"
             "'ON':['=',['.a.name'],['.b.name']],'SCOPE':'scope'}],'WHAT':[['.a.x']],"
             "'WHERE':['MATCH()','a.ftsIndex',['.b.y']]}");
    CHECK(translate("SELECT a.x FROM coll a JOIN scope.coll b ON a.name = b.name "
                    "WHERE MATCH(b.ftsIndex, a.y)")
          == "{'FROM':[{'AS':'a','COLLECTION':'coll'},{'AS':'b','COLLECTION':'coll','JOIN':'INNER',"
             "'ON':['=',['.a.name'],['.b.name']],'SCOPE':'scope'}],'WHAT':[['.a.x']],"
             "'WHERE':['MATCH()','b.ftsIndex',['.a.y']]}");
    // ftsIndex does not have to be qualified by collection alias if all aliases refer to
    // the same collection.
    CHECK(translate("SELECT a.x FROM coll a JOIN coll b ON a.name = b.y WHERE MATCH(ftsIndex, b.y)")
          == "{'FROM':[{'AS':'a','COLLECTION':'coll'},{'AS':'b','COLLECTION':'coll','JOIN':'INNER',"
             "'ON':['=',['.a.name'],['.b.y']]}],'WHAT':[['.a.x']],'WHERE':['MATCH()','ftsIndex',['.b.y']]}");
    {
        ExpectingExceptions x;
        // a and b refer to different collections, and, hence, ftsIndex must preceded by an alias.
        CHECK_THROWS_WITH(translate("SELECT a.x FROM coll a JOIN scope.coll b ON "
                                    "a.name = b.y WHERE MATCH(ftsIndex, b.y)"),
                          "property 'ftsIndex' does not begin with a declared 'AS' alias");
        CHECK_THROWS_WITH(translate("SELECT a.x FROM coll a JOIN scope.coll b ON "
                                    "a.name = b.y WHERE MATCH(c.ftsIndex, b.y)"),
                          "property 'c.ftsIndex' does not begin with a declared 'AS' alias");
    }

    // database aliases can be quoted.
    CHECK(translate("SELECT `first.collection`.x FROM coll AS `first.collection` "
                    "JOIN scope.coll `second.collection` ON `first.collection`.name = `second.collection`.y "
                    "WHERE MATCH(`first.collection`.ftsIndex, `second.collection`.y)")
          == "{'FROM':[{'AS':'first\\\\.collection','COLLECTION':'coll'},"
             "{'AS':'second\\\\.collection','COLLECTION':'coll','JOIN':'INNER',"
             "'ON':['=',['.first\\\\.collection.name'],['.second\\\\.collection.y']],'SCOPE':'scope'}],"
             "'WHAT':[['.first\\\\.collection.x']],"
             "'WHERE':['MATCH()','first\\\\.collection.ftsIndex',['.second\\\\.collection.y']]}");
    CHECK(translate("SELECT coll.x FROM coll JOIN scope.coll ON coll.name = scope.coll.y "
                    "WHERE MATCH(coll.ftsIndex, scope.coll.y)")
          == "{'FROM':[{'COLLECTION':'coll'},{'COLLECTION':'coll','JOIN':'INNER',"
             "'ON':['=',['.coll.name'],['.scope.coll.y']],'SCOPE':'scope'}],"
             "'WHAT':[['.coll.x']],'WHERE':['MATCH()','coll.ftsIndex',['.scope.coll.y']]}");
    CHECK(translate("SELECT scope.coll.x FROM scope.coll JOIN coll ON scope.coll.name = coll.y "
                    "WHERE MATCH(`scope.coll`.ftsIndex, coll.y)")
          == "{'FROM':[{'COLLECTION':'coll','SCOPE':'scope'},{'COLLECTION':'coll','JOIN':'INNER',"
             "'ON':['=',['.scope.coll.name'],['.coll.y']]}],"
             "'WHAT':[['.scope.coll.x']],'WHERE':['MATCH()','scope\\\\.coll.ftsIndex',['.coll.y']]}");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL Performance", "[Query][N1QL][C]") {
    const char* n1ql = nullptr;
    string      buf;
    double      checkBound = std::numeric_limits<double>::max();

    SECTION("Very Long Query") {
        // 3b1fe0d6fe46a5a4e1655dbe6f42e89154a189dd, this query takes 4 seconds
        n1ql       = "SELECT doc.* FROM _ doc WHERE "
                     "doc.type = 'Model' AND "
                     "doc.s NOT IN ('A', 'B', 'V') AND "
                     "((doc.model.total.totalA "
                     "- ifnull(doc.model.totalA.totalB, 0)) "
                     "> 0 OR doc.t = false) AND "
                     "(doc.q IS NULL OR "
                     "ifnull(doc.q.e, 'e') = 'e' AND "
                     "ifnull(doc.q.m, 0) == 0)";
        checkBound = 0.5;
    }

    Stopwatch sw;
    string    json    = translate(n1ql);
    double    elapsed = sw.elapsed();
    cerr << "\t\tElapsed time/check time = " << elapsed << "/" << checkBound << endl;
    CHECK(elapsed < checkBound);
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL DateTime", "[Query][N1QL]") {
    // millis
    CHECK(translate("SELECT MILLIS_TO_UTC(1540319581000) AS RESULT")
          == "{'WHAT':[['AS',['MILLIS_TO_UTC()',1540319581000],'RESULT']]}");
    // millis, fmt
    CHECK(translate("SELECT MILLIS_TO_UTC(1540319581000,'1111-11-11') AS RESULT")
          == "{'WHAT':[['AS',['MILLIS_TO_UTC()',1540319581000,'1111-11-11'],'RESULT']]}");
    // millis, tz
    CHECK(translate("SELECT MILLIS_TO_TZ(1540319581000, 500) AS RESULT")
          == "{'WHAT':[['AS',['MILLIS_TO_TZ()',1540319581000,500],'RESULT']]}");
    // millis, tz, fmt
    CHECK(translate("SELECT MILLIS_TO_TZ(1540319581000, 500, '1111-11-11') AS RESULT")
          == "{'WHAT':[['AS',['MILLIS_TO_TZ()',1540319581000,500,'1111-11-11'],'RESULT']]}");
    // millis
    CHECK(translate("SELECT MILLIS_TO_STR(1540319581000) AS RESULT")
          == "{'WHAT':[['AS',['MILLIS_TO_STR()',1540319581000],'RESULT']]}");
    // millis, fmt
    CHECK(translate("SELECT MILLIS_TO_STR(1540319581000,'1111-11-11') AS RESULT")
          == "{'WHAT':[['AS',['MILLIS_TO_STR()',1540319581000,'1111-11-11'],'RESULT']]}");
    // date
    CHECK(translate("SELECT STR_TO_MILLIS('2018-10-23T18:33:01Z') AS RESULT")
          == "{'WHAT':[['AS',['STR_TO_MILLIS()','2018-10-23T18:33:01Z'],'RESULT']]}");
    // date
    CHECK(translate("SELECT STR_TO_UTC('2018-10-23T18:33:01Z') AS RESULT")
          == "{'WHAT':[['AS',['STR_TO_UTC()','2018-10-23T18:33:01Z'],'RESULT']]}");
    // date, fmt
    CHECK(translate("SELECT STR_TO_UTC('2018-10-23T18:33:01Z','1111-11-11') AS RESULT")
          == "{'WHAT':[['AS',['STR_TO_UTC()','2018-10-23T18:33:01Z','1111-11-11'],'RESULT']]}");
    // date, tz
    CHECK(translate("SELECT STR_TO_TZ('2018-10-23T18:33:01Z', 500) AS RESULT")
          == "{'WHAT':[['AS',['STR_TO_TZ()','2018-10-23T18:33:01Z',500],'RESULT']]}");
    // date, tz, fmt
    CHECK(translate("SELECT STR_TO_TZ('2018-10-23T18:33:01Z', 500, '1111-11-11') AS RESULT")
          == "{'WHAT':[['AS',['STR_TO_TZ()','2018-10-23T18:33:01Z',500,'1111-11-11'],'RESULT']]}");
    // date, date, component
    CHECK(translate("SELECT DATE_DIFF_STR('2018-10-23','2018-10-24','day') AS RESULT")
          == "{'WHAT':[['AS',['DATE_DIFF_STR()','2018-10-23','2018-10-24','day'],'RESULT']]}");
    // millis, millis, component
    CHECK(translate("SELECT DATE_DIFF_MILLIS(1540319581000,1540405981000,'day') AS RESULT")
          == "{'WHAT':[['AS',['DATE_DIFF_MILLIS()',1540319581000,1540405981000,'day'],'RESULT']]}");
    // date, amount, component
    CHECK(translate("SELECT DATE_ADD_STR('2018-10-23T18:33:01Z',3,'day') AS RESULT")
          == "{'WHAT':[['AS',['DATE_ADD_STR()','2018-10-23T18:33:01Z',3,'day'],'RESULT']]}");
    // date, amount, component, fmt
    CHECK(translate("SELECT DATE_ADD_STR('2018-10-23T18:33:01Z',3,'day','1111-11-11') AS RESULT")
          == "{'WHAT':[['AS',['DATE_ADD_STR()','2018-10-23T18:33:01Z',3,'day','1111-11-11'],'RESULT']]}");
    // millis, amount, component
    CHECK(translate("SELECT DATE_ADD_MILLIS(1540319581000,3,'day') AS RESULT")
          == "{'WHAT':[['AS',['DATE_ADD_MILLIS()',1540319581000,3,'day'],'RESULT']]}");
}

#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(N1QLParserTest, "N1QL Vector Search", "[Query][N1QL][VectorSearch]") {
    tableNames.emplace("kv_default:vector:vecIndex");
    tableNames.emplace("kv_.coll");
    tableNames.emplace("kv_.coll:vector:vecIndex");
    tableNames.emplace("kv_.scope.coll");
    tableNames.emplace("kv_.scope.coll:vector:vecIndex");
    tableNames.emplace("kv_.other");

    CHECK(translate("SELECT VECTOR_DISTANCE(a.vecIndex, $target) AS distance "
                    "FROM _default AS a JOIN other ON META(a).id = other.refID "
                    "ORDER BY distance LIMIT 100")
          == "{'FROM':[{'AS':'a','COLLECTION':'_default'},"
             "{'COLLECTION':'other','JOIN':'INNER','ON':['=',['_.',['meta()','a'],'.id'],['.other.refID']]}],"
             "'LIMIT':100,"
             "'ORDER_BY':[['.distance']],"
             "'WHAT':[['AS',['VECTOR_DISTANCE()','a.vecIndex',['$target']],'distance']]}");

    CHECK(translate("SELECT META().id, VECTOR_DISTANCE(vecIndex, $target) AS distance ORDER BY distance LIMIT 5")
          == "{'LIMIT':5,'ORDER_BY':[['.distance']],'WHAT':[['_.',['meta()'],'.id'],"
             "['AS',['VECTOR_DISTANCE()','vecIndex',['$target']],'distance']]}");

    CHECK(translate("SELECT META().id, VECTOR_DISTANCE(coll.vecIndex, $target) AS distance FROM coll "
                    "ORDER BY distance LIMIT 5")
          == "{'FROM':[{'COLLECTION':'coll'}],'LIMIT':5,"
             "'ORDER_BY':[['.distance']],'WHAT':[['_.',['meta()'],'.id'],"
             "['AS',['VECTOR_DISTANCE()','coll.vecIndex',['$target']],'distance']]}");

    CHECK(translate("SELECT META().id, VECTOR_DISTANCE(C.vecIndex, $target) AS distance "
                    "FROM scope.coll C "
                    "ORDER BY distance LIMIT 99")
          == "{'FROM':[{'AS':'C','COLLECTION':'coll','SCOPE':'scope'}],'LIMIT':99,"
             "'ORDER_BY':[['.distance']],'WHAT':[['_.',['meta()'],'.id'],"
             "['AS',['VECTOR_DISTANCE()','C.vecIndex',['$target']],'distance']]}");

    CHECK(translate("SELECT META().id, VECTOR_DISTANCE(vecIndex, $target) AS distance "
                    "FROM scope.coll C "
                    "ORDER BY distance LIMIT 456")
          == "{'FROM':[{'AS':'C','COLLECTION':'coll','SCOPE':'scope'}],'LIMIT':456,"
             "'ORDER_BY':[['.distance']],'WHAT':[['_.',['meta()'],'.id'],"
             "['AS',['VECTOR_DISTANCE()','vecIndex',['$target']],'distance']]}");
}
#endif
