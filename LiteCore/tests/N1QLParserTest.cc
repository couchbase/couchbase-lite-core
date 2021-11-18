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
#include "n1ql_parser.hh"
#include "StringUtil.hh"
#include "fleece/Mutable.hh"
#include <iostream>


using namespace std;
using namespace litecore;
using namespace fleece;

class N1QLParserTest : public QueryParserTest {
protected:

    // Translates N1QL to JSON, with strings single-quoted to avoid tons of escapes in the tests.
    // On syntax error, returns "".
    string translate(const char *n1ql) {
        UNSCOPED_INFO("N1QL: " << n1ql);
        unsigned errorPos;

        FLValue dict = (FLValue) n1ql::parse(n1ql, &errorPos);
        if (!dict) {
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
    CHECK(translate("SELECT {'x':17, 'null': null,'empty':{} , 'str':'hi'||'there'}") == "{'WHAT':[{'empty':{},'null':null,'str':['||','hi','there'],'x':17}]}");
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
    CHECK(translate("select meta(id).id from _default as id") == "{'FROM':[{'AS':'id','COLLECTION':'_default'}],"
          "'WHAT':[['_.',['meta()','id'],'.id']]}");
    CHECK(translate("select meta().sequence") == "{'WHAT':[['_.',['meta()'],'.sequence']]}");
    CHECK(translate("select meta().revisionID") == "{'WHAT':[['_.',['meta()'],'.revisionID']]}");
    CHECK(translate("select meta().deleted") == "{'WHAT':[['_.',['meta()'],'.deleted']]}");
    CHECK(translate("select meta(_default).id from _default") == "{'FROM':[{'COLLECTION':'_default'}],'WHAT':[['_.',['meta()','_default'],'.id']]}");
    {
        ExpectingExceptions x;
        CHECK_THROWS_WITH(translate("select meta().bogus"), "'bogus' is not a valid Meta key");
        CHECK_THROWS_WITH(translate("select meta(_default).bogus from _default"), "'bogus' is not a valid Meta key");
        CHECK_THROWS_WITH(translate("select meta(id).id as id"), "database alias 'id' does not match a declared 'AS' alias");
    }
    CHECK(translate("select foo[17]") == "{'WHAT':[['.foo[17]']]}");
    CHECK(translate("select foo.bar[-1].baz") == "{'WHAT':[['.foo.bar[-1].baz']]}");

    CHECK(translate("SELECT *") == "{'WHAT':[['.']]}");
    CHECK(translate("SELECT db.*") == "{'WHAT':[['.db.']]}");

    CHECK(translate("select $var") == "{'WHAT':[['$var']]}");

    // "custId" is implicitly scoped by the unique alias, "orders".
    CHECK(translate("SELECT DISTINCT custId FROM _default AS orders where test_id = 'agg_func' ORDER BY custId") ==
          "{'DISTINCT':true,'FROM':[{'AS':'orders','COLLECTION':'_default'}],'ORDER_BY':[['.custId']],"
          "'WHAT':[['.custId']],'WHERE':['=',['.test_id'],'agg_func']}");
    {
        ExpectingExceptions x;
        CHECK_THROWS_WITH(translate("SELECT custId, other.custId FROM _default AS orders JOIN _default as other "
                                    "ON orders.test_id = other.test_id ORDER BY custId"),
                          "property 'custId.' does not begin with a declared 'AS' alias");
    }
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

    CHECK(translate("SELECT 3+4) from stuff") == "");

    CHECK(translate("SELECT 17 IN (1, 2, 3)") == "{'WHAT':[['IN',17,['[]',1,2,3]]]}");
    CHECK(translate("SELECT 17 NOT IN (1, 2, 3)") == "{'WHAT':[['NOT IN',17,['[]',1,2,3]]]}");

    CHECK(translate("SELECT 6 IS 9") == "{'WHAT':[['IS',6,9]]}");
    CHECK(translate("SELECT 6 IS NOT 9") == "{'WHAT':[['IS NOT',6,9]]}");
    CHECK(translate("SELECT 6 NOT NULL") == "{'WHAT':[['IS NOT',6,null]]}");
    CHECK(translate("SELECT 6 WHERE x IS   NOT   VALUED") == "{'WHAT':[6],'WHERE':['NOT',['IS VALUED',['.x']]]}");
    CHECK(translate("SELECT 6 WHERE x  IS  VALUED") == "{'WHAT':[6],'WHERE':['IS VALUED',['.x']]}");
    
    CHECK(translate("SELECT 'foo' LIKE 'f%'") == "{'WHAT':[['LIKE','foo','f%']]}");
    CHECK(translate("SELECT 'foo' NOT LIKE 'f%'") == "{'WHAT':[['NOT',['LIKE','foo','f%']]]}");
    CHECK(translate("SELECT 1 WHERE MATCH(text, 'word') ORDER BY RANK(text)") ==
          "{'ORDER_BY':[['RANK()','text']],'WHAT':[1],'WHERE':['MATCH()','text','word']}");
    CHECK(translate("SELECT 1 WHERE MATCH(`text`, 'word')") == "{'WHAT':[1],'WHERE':['MATCH()','text','word']}");
    CHECK(translate("SELECT 1 WHERE MATCH('text', 'word')") == ""); // The first argument to MATCH must be an identifier.
    CHECK(translate("SELECT 1 WHERE MATCH(text, 'word') ORDER BY RANK('text')") == ""); // The argument to RANK must be an identifier.
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

    CHECK(translate("SELECT product.categories CATG, COUNT(*) AS numprods WHERE test_id = \"agg_func\" "
                    "GROUP BY product.categories HAVING COUNT(*) BETWEEN 15 and 30 ORDER BY CATG, numprods LIMIT 3")
          == "{'GROUP_BY':[['.product.categories']],"
             "'HAVING':['BETWEEN',['COUNT()',['.']],15,30],"
             "'LIMIT':3,"
             "'ORDER_BY':[['.CATG'],['.numprods']],"
             "'WHAT':[['AS',['.product.categories'],'CATG'],['AS',['COUNT()',['.']],'numprods']],"
             "'WHERE':['=',['.test_id'],'agg_func']}");
    CHECK(translate("SELECT product.categories CATG, COUNT ( * ) AS numprods WHERE test_id = \"agg_func\" "
                    "GROUP BY product.categories HAVING COUNT(*) BETWEEN POWER ( ABS(-2) , ABS(3) ) and 30 ORDER BY CATG, numprods LIMIT 3")
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
    CHECK(translate("SELECT (name = 'fred') COLLATE (UNICODE CASE NODIAC)") == "{'WHAT':[['COLLATE',{'CASE':true,'DIAC':false,'UNICODE':true},['=',['.name'],'fred']]]}");
    CHECK(translate("SELECT (name = 'fred') COLLATE UNICODE NOCASE") == "");
    CHECK(translate("SELECT (name = 'fred') COLLATE (NOCASE FRED)") == "");
    CHECK(translate("SELECT (name = 'fred') COLLATE NOCASE FRED")
          == "{'WHAT':[['AS',['COLLATE',{'CASE':false},['=',['.name'],'fred']],'FRED']]}");
    CHECK(translate("SELECT (name = 'fred') COLLATE (NOCASE) FRED")
          == "{'WHAT':[['AS',['COLLATE',{'CASE':false},['=',['.name'],'fred']],'FRED']]}");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL SELECT", "[Query][N1QL][C]") {
    CHECK(translate("SELECT foo") == "{'WHAT':[['.foo']]}");
    CHECK(translate("SELECT ALL foo") == "{'WHAT':[['.foo']]}");
    CHECK(translate("SELECT DISTINCT foo") == "{'DISTINCT':true,'WHAT':[['.foo']]}");

    CHECK(translate("SELECT foo bar") == "{'WHAT':[['AS',['.foo'],'bar']]}");
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
    CHECK(translate("SELECT foo OFFSET 20 LIMIT 10") == "{'LIMIT':10,'OFFSET':20,'WHAT':[['.foo']]}");
    CHECK(translate("SELECT orderlines[0] WHERE test_id='order_func' ORDER BY orderlines[0].productId, orderlines[0].qty ASC OFFSET 8192 LIMIT 1")
          == "{'LIMIT':1,'OFFSET':8192,'ORDER_BY':[['.orderlines[0].productId'],"
             "['ASC',['.orderlines[0].qty']]],'WHAT':[['.orderlines[0]']],'WHERE':['=',['.test_id'],'order_func']}");

    CHECK(translate("SELECT foo FROM _") == "{'FROM':[{'COLLECTION':'_'}],'WHAT':[['.foo']]}");
    CHECK(translate("SELECT foo FROM _default") == "{'FROM':[{'COLLECTION':'_default'}],'WHAT':[['.foo']]}");

// QueryParser does not support "IN SELECT" yet
//    CHECK(translate("SELECT 17 NOT IN (SELECT value WHERE type='prime')") == "{'WHAT':[['NOT IN',17,['SELECT',{'WHAT':[['.value']],'WHERE':['=',['.type'],'prime']}]]]}");

    tableNames.insert("kv_coll_product");

    CHECK(translate("SELECT productId, color, categories WHERE categories[0] LIKE 'Bed%' AND test_id='where_func' ORDER BY productId LIMIT 3") == "{'LIMIT':3,'ORDER_BY':[['.productId']],'WHAT':[['.productId'],['.color'],['.categories']],'WHERE':['AND',['LIKE',['.categories[0]'],'Bed%'],['=',['.test_id'],'where_func']]}");
    CHECK(translate("SELECT FLOOR(unitPrice+0.5) as sc FROM product where test_id = \"numberfunc\" ORDER BY sc limit 5") ==
          "{'FROM':[{'COLLECTION':'product'}],'LIMIT':5,'ORDER_BY':[['.sc']],"
          "'WHAT':[['AS',['FLOOR()',['+',['.unitPrice'],0.5]],'sc']],'WHERE':['=',['.test_id'],'numberfunc']}");

    CHECK(translate("SELECT META().id AS id WHERE META().id = $ID") ==
          "{'WHAT':[['AS',['_.',['meta()'],'.id'],'id']],'WHERE':['=',['_.',['meta()'],'.id'],['$ID']]}");
    CHECK(translate("SELECT META().id AS id WHERE id = $ID") ==
          "{'WHAT':[['AS',['_.',['meta()'],'.id'],'id']],'WHERE':['=',['.id'],['$ID']]}");
}

TEST_CASE_METHOD(N1QLParserTest, "N1QL JOIN", "[Query][N1QL][C]") {
    tableNames.insert("kv_coll_db");
    tableNames.insert("kv_coll_other");
    tableNames.insert("kv_coll_x");

    CHECK(translate("SELECT 0 FROM db") == "{'FROM':[{'COLLECTION':'db'}],'WHAT':[0]}");
    CHECK(translate("SELECT * FROM db") == "{'FROM':[{'COLLECTION':'db'}],'WHAT':[['.']]}");
    CHECK(translate("SELECT file.name FROM db AS file") == "{'FROM':[{'AS':'file','COLLECTION':'db'}],'WHAT':[['.file.name']]}");
    CHECK(translate("SELECT file.name FROM db file")                    // omit 'AS'
          == "{'FROM':[{'AS':'file','COLLECTION':'db'}],"
             "'WHAT':[['.file.name']]}");
    CHECK(translate("SELECT db.name FROM db JOIN other ON other.key = db.key")
          == "{'FROM':[{'COLLECTION':'db'},"
                      "{'COLLECTION':'other','JOIN':'INNER','ON':['=',['.other.key'],['.db.key']]}],"
              "'WHAT':[['.db.name']]}");
    CHECK(translate("SELECT db.name FROM db JOIN x other ON other.key = db.key") // omit 'AS'
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
    CHECK(translate("SELECT a, b, c FROM db a JOIN other b ON (a.n = b.n) JOIN x c ON (b.m = c.m) WHERE a.type = b.type AND b.type = c.type")
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
    CHECK(translate("SELECT is_array(x),  is_atom(x),  is_boolean(x),  is_number(x),  is_object(x),  is_string(x),  typename(x)")
          == "{'WHAT':[['is_array()',['.x']],['is_atom()',['.x']],['is_boolean()',['.x']],['is_number()',['.x']],"
             "['is_object()',['.x']],['is_string()',['.x']],['typename()',['.x']]]}");
    CHECK(translate("SELECT toarray(x),  toatom(x),  toboolean(x),  tonumber(x),  toobject(x),  tostring(x)")
          == "{'WHAT':[['toarray()',['.x']],['toatom()',['.x']],['toboolean()',['.x']],['tonumber()',['.x']],"
             "['toobject()',['.x']],['tostring()',['.x']]]}");
    CHECK(translate("SELECT to_array(x),  to_atom(x),  to_boolean(x),  to_number(x),  to_object(x),  to_string(x)")
          == "{'WHAT':[['to_array()',['.x']],['to_atom()',['.x']],['to_boolean()',['.x']],['to_number()',['.x']],"
             "['to_object()',['.x']],['to_string()',['.x']]]}");
}
