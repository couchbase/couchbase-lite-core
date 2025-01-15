//
// Created by Callum Birks on 17/10/2024.
//

#include "c4Test.hh"

#include "c4Collection.h"
#include "c4Database.hh"
#include "c4Index.h"
#include "StringUtil.hh"
#include "c4Query.h"
#include "c4Query.hh"

class ArrayIndexTest : public C4Test {
  public:
    ArrayIndexTest() : C4Test() {}

    static void importTestData(C4Collection* collection) {
        importJSONLines(sFixturesDir + "profiles_100.json", collection);
    }

    /**
     * Import docs from individual JSON strings. The docIDs are created as { `%07u`, i + 1 }, where i is the index in the
     * list of docs. So the first doc is `0000001`, the second is `0000002`, etc.
     *
     * If you want to import docs with ID `0000034`, `0000035`, you should set firstDocNum to `34`.
     */
    void importJSONDocs(C4Collection* collection, std::initializer_list<std::string_view> docs,
                        int firstDocNum = 1) const {
        TransactionHelper t(db);
        for ( auto jsonDoc : docs ) {
            constexpr size_t bufSize = 10;
            char             docID[bufSize];
            snprintf(docID, bufSize, "%07u", firstDocNum++);

            C4Log("--- Importing doc '%s' %s", docID, jsonDoc.data());

            c4::ref doc = c4coll_createDoc(collection, c4str(docID), json2fleece(jsonDoc.data()), 0, ERROR_INFO());
            REQUIRE(doc);
        }
    }

    void updateDoc(C4Collection* collection, slice docID, std::string_view jsonBody) const {
        TransactionHelper t(db);
        c4::ref           doc = c4coll_getDoc(collection, docID, true, kDocGetCurrentRev, ERROR_INFO());
        REQUIRE(doc);
        doc = c4doc_update(doc, json2fleece(jsonBody.data()), 0, ERROR_INFO());
        REQUIRE(doc);
        bool result = c4doc_save(doc, 0, ERROR_INFO());
        REQUIRE(result);
    }

    void deleteDoc(C4Collection* collection, slice docID) const {
        TransactionHelper t(db);
        c4::ref           doc = c4coll_getDoc(collection, docID, true, kDocGetCurrentRev, ERROR_INFO());
        REQUIRE(doc);
        doc = c4doc_update(doc, nullslice, doc->selectedRev.flags | kRevDeleted, ERROR_INFO());
        REQUIRE(doc);
        bool result = c4doc_save(doc, 0, ERROR_INFO());
        REQUIRE(result);
    }

    /**
     * Given a newly-created C4QueryEnumerator (from c4query_run), check the resulting rows and columns match the
     * `expectedResults`.
     * `expectedResults` is a list of JSON strings. Each JSON string should be a JSON array of the expected columns,
     * in the order they are declared in the `SELECT` statement of the query.
     */
    void validateQuery(C4QueryEnumerator* queryenum, std::initializer_list<std::string_view> expectedResults) const {
        REQUIRE(queryenum);

        {  // debug log the actual and expected query rows
            const std::string_view* expected = expectedResults.begin();
            std::stringstream       ss{};
            bool                    hasExpected = expected != expectedResults.end();
            while ( c4queryenum_next(queryenum, nullptr) ) {
                FLArrayIterator columnsIter = queryenum->columns;
                for ( int col = 0; col < FLArrayIterator_GetCount(&columnsIter); col++ ) {
                    FLValue val = FLArrayIterator_GetValueAt(&columnsIter, col);
                    if ( col > 0 ) ss << ", ";
                    ss << Value(val).toJSONString();
                }
                ss << " <- ACTUAL | EXPECTED -> " << (hasExpected ? expected->data() : "") << '\n';
                if ( hasExpected ) hasExpected = ++expected != expectedResults.end();
            }
            std::string sstr = ss.str();
            C4Log("VALIDATING ARRAY INDEX QUERY:\n%s", sstr.c_str());
            REQUIRE(c4queryenum_restart(queryenum, nullptr));
        }

        REQUIRE(c4queryenum_getRowCount(queryenum, nullptr) == expectedResults.size());

        for ( auto& expectedJson : expectedResults ) {
            {
                bool hasNext = c4queryenum_next(queryenum, ERROR_INFO());
                REQUIRE(hasNext);
            }

            Doc   expectedFleece = json2dict(expectedJson.data());
            Array expectedArray  = expectedFleece.asArray();

            FLArrayIterator columns = queryenum->columns;
            REQUIRE(expectedArray.count() == FLArrayIterator_GetCount(&columns));
            for ( int i = 0; i < expectedArray.count(); i++ ) {
                FLValue val           = FLArrayIterator_GetValueAt(&columns, i);
                slice   expectedSlice = expectedArray[i].asString();
                slice   actualSlice   = FLValue_AsString(val);
                if ( expectedSlice != actualSlice ) {}
                REQUIRE(FLValue_IsEqual(val, expectedArray[i]));
            }
        }
    }
};

static bool createArrayIndex(C4Collection* coll, slice name, slice expression, const char* path, C4Error* err) {
    C4IndexOptions options{nullptr, false, false, nullptr, path};
    return c4coll_createIndex(coll, name, expression, kC4JSONQuery, kC4ArrayIndex, &options, err);
}

constexpr std::string_view p0001 =
        R"({"pid": "p-0001", "name": {"first": "Lue", "last": "Laserna"}, "contacts": [{"address": {"city": "San Pedro", "state": "CA", "street": "19 Deer Loop", "zip": "90732"}, "emails": ["lue.laserna@nosql-matters.org", "laserna@nosql-matters.org"], "phones": [{"numbers": ["310-8268551", "310-7618427"], "preferred": false, "type": "home"}, {"numbers": ["310-9601308"], "preferred": true, "type": "mobile"}], "type": "primary"}, {"address": {"city": "San Pedro", "state": "CA", "street": "1820 Maple Ln", "zip": "90732"}, "emails": ["Lue@email.com", "Laserna@email.com"], "phones": [{"numbers": ["310-6653153"], "preferred": false, "type": "home"}, {"numbers": ["310-4833623"], "preferred": true, "type": "mobile"}], "type": "secondary"}], "likes": ["chatting"]})";
constexpr std::string_view p0002 =
        R"({"pid": "p-0002", "name": {"first": "Jasper", "last": "Grebel"}, "contacts": [{"address": {"city": "Burns", "state": "KS", "street": "19 Florida Loop", "zip": "66840"}, "emails": ["jasper.grebel@nosql-matters.org"], "phones": [{"numbers": ["316-2417120", "316-2767391"], "preferred": false, "type": "home"}, {"numbers": ["316-8833161"], "preferred": true, "type": "mobile"}], "type": "primary"}, {"address": {"city": "Burns", "state": "KS", "street": "4795 Willow Loop", "zip": "66840"}, "emails": ["Jasper@email.com", "Grebel@email.com"], "phones": [{"numbers": ["316-9487549"], "preferred": true, "type": "home"}, {"numbers": ["316-4737548"], "preferred": false, "type": "mobile"}], "type": "secondary"}], "likes": ["shopping"]})";
constexpr std::string_view p0003 =
        R"({"pid": "p-0003", "name": {"first": "Kandra", "last": "Beichner"}, "contacts": [{"address": {"city": "Tacoma", "state": "WA", "street": "6 John Run", "zip": "98434"}, "emails": ["kandra.beichner@nosql-matters.org", "kandra@nosql-matters.org"], "phones": [{"numbers": ["253-0405964"], "preferred": false, "type": "home"}, {"numbers": ["253-7421842"], "preferred": true, "type": "mobile"}], "type": "primary"}, {"address": {"city": "Tacoma", "state": "WA", "street": "9509 Cedar Ave", "zip": "98434"}, "emails": ["Kandra@email.com", "Beichner@email.com"], "phones": [{"numbers": ["253-5727806"], "preferred": false, "type": "home"}, {"numbers": ["253-8671217"], "preferred": true, "type": "mobile"}], "type": "secondary"}], "likes": ["swimming"]})";
constexpr std::string_view p0004 =
        R"({"pid": "p-0004", "name": {"first": "Jeff", "last": "Schmith"}, "contacts": [{"address": {"city": "Poughkeepsie", "state": "AR", "street": "14 198th St", "zip": "72569"}, "emails": ["jeff.schmith@nosql-matters.org"], "phones": [{"numbers": [], "preferred": false, "type": "home"}, {"numbers": ["870-5974023"], "preferred": true, "type": "mobile"}], "type": "primary"}, {"address": {"city": "Poughkeepsie", "state": "AR", "street": "9356 Willow Cir", "zip": "72569"}, "emails": ["Jeff@email.com", "Schmith@email.com"], "phones": [{"numbers": ["870-4182309"], "preferred": true, "type": "home"}, {"numbers": ["870-1205865"], "preferred": false, "type": "mobile"}], "type": "secondary"}], "likes": ["chatting", "boxing", "reading"]})";

// 1. TestCreateArrayIndexWithEmptyPath
TEST_CASE_METHOD(ArrayIndexTest, "Create Array Index with Empty Path", "[C][ArrayIndex]") {
    const auto defaultColl = REQUIRED(c4db_getDefaultCollection(db, ERROR_INFO()));
    C4Error    err{};
    createArrayIndex(defaultColl, "arridx"_sl, nullslice, "", &err);
    CHECK(err.code == kC4ErrorInvalidQuery);
}

// 2. TestCreateArrayIndexWithInvalidExpressions
TEST_CASE_METHOD(ArrayIndexTest, "Create Array Index with Invalid Expressions", "[C][ArrayIndex]") {
    const auto defaultColl = REQUIRED(c4db_getDefaultCollection(db, ERROR_INFO()));
    C4Error    err{};

    createArrayIndex(defaultColl, "arridx"_sl, R"([".address.state", "", ".address.city"])", "contacts", &err);
    CHECK(err.code == kC4ErrorInvalidQuery);

    createArrayIndex(defaultColl, "arridx"_sl, R"([".address.state", , ".address.city"])", "contacts", &err);
    CHECK(err.code == kC4ErrorInvalidQuery);
}

// 3. TestCreateUpdateDeleteArrayIndexSingleLevel
TEST_CASE_METHOD(ArrayIndexTest, "CRUD Array Index Single Level", "[C][ArrayIndex]") {
    C4Collection* coll    = createCollection(db, {"profiles"_sl, "_default"_sl});
    bool          created = createArrayIndex(coll, "contacts"_sl, R"([".address.state"])", "contacts", ERROR_INFO());
    REQUIRE(created);
    c4::ref query = c4query_new2(
            db, kC4N1QLQuery,
            "SELECT p.pid, c.address.city, c.address.state FROM profiles AS p UNNEST p.contacts AS c WHERE c.address.state IS valued ORDER BY p.pid"_sl,
            nullptr, ERROR_INFO());
    REQUIRE(query);

    c4::ref queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    REQUIRE(c4queryenum_getRowCount(queryenum, nullptr) == 0);

    importJSONDocs(coll, {p0001, p0002, p0003});
    queryenum = c4query_run(query, nullslice, ERROR_INFO());
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0002", "Burns", "KS"])",
                                     R"(["p-0002", "Burns", "KS"])",
                                     R"(["p-0003", "Tacoma", "WA"])",
                                     R"(["p-0003", "Tacoma", "WA"])",
                             });

    importJSONDocs(coll, {p0004}, 4);
    deleteDoc(coll, "0000003"_sl);
    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0002", "Burns", "KS"])",
                                     R"(["p-0002", "Burns", "KS"])",
                                     R"(["p-0004", "Poughkeepsie", "AR"])",
                                     R"(["p-0004", "Poughkeepsie", "AR"])",
                             });

    // p0001 with an extra contact
    constexpr std::string_view p0001_update =
            R"({"pid":"p-0001","name":{"first":"Lue","last":"Laserna"},"contacts":[{"address":{"city":"San Pedro","state":"CA","street":"19 Deer Loop","zip":"90732"},"emails":["lue.laserna@nosql-matters.org","laserna@nosql-matters.org"],"phones":[{"numbers":["310-8268551","310-7618427"],"preferred":false,"type":"home"},{"numbers":["310-9601308"],"preferred":true,"type":"mobile"}],"type":"primary"},{"address":{"city":"San Pedro","state":"CA","street":"1820 Maple Ln","zip":"90732"},"emails":["Lue@email.com","Laserna@email.com"],"phones":[{"numbers":["310-6653153"],"preferred":false,"type":"home"},{"numbers":["310-4833623"],"preferred":true,"type":"mobile"}],"type":"secondary"},{"address":{"city":"Houston","state":"TX","street":"4203 Greenhouse Rd","zip":"77084"},"emails":["fawkes@nosql-matters.org"],"phones":[{"numbers":["979-452-6018","903-272-0111"],"preferred":false,"type":"home"},{"numbers":["817-659-7206"],"preferred":true,"type":"mobile"}],"type":"primary"}],"likes":["chatting"]})";

    // p0002 with a secondary contact removed
    constexpr std::string_view p0002_update =
            R"({"pid":"p-0002","name":{"first":"Jasper","last":"Grebel"},"contacts":[{"address":{"city":"Burns","state":"KS","street":"19 Florida Loop","zip":"66840"},"emails":["jasper.grebel@nosql-matters.org"],"phones":[{"numbers":["316-2417120","316-2767391"],"preferred":false,"type":"home"},{"numbers":["316-8833161"],"preferred":true,"type":"mobile"}],"type":"primary"}],"likes":["shopping"]})";

    updateDoc(coll, "0000001"_sl, p0001_update);
    updateDoc(coll, "0000002"_sl, p0002_update);

    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0001", "Houston", "TX"])",
                                     R"(["p-0002", "Burns", "KS"])",
                                     R"(["p-0004", "Poughkeepsie", "AR"])",
                                     R"(["p-0004", "Poughkeepsie", "AR"])",
                             });

    bool deleted = c4coll_deleteIndex(coll, "contacts"_sl, ERROR_INFO());
    REQUIRE(deleted);

    query = c4query_new2(
            db, kC4N1QLQuery,
            "SELECT p.pid, c.address.city, c.address.state FROM profiles AS p UNNEST p.contacts AS c WHERE c.address.state IS valued ORDER BY p.pid"_sl,
            nullptr, ERROR_INFO());
    REQUIRE(query);

    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0001", "Houston", "TX"])",
                                     R"(["p-0002", "Burns", "KS"])",
                                     R"(["p-0004", "Poughkeepsie", "AR"])",
                                     R"(["p-0004", "Poughkeepsie", "AR"])",
                             });
}

// 4. TestCreateUpdateDeleteNestedArrayIndex
TEST_CASE_METHOD(ArrayIndexTest, "CRUD Nested Array Index", "[C][ArrayIndex]") {
    C4Collection* coll    = createCollection(db, {"profiles"_sl, "_default"_sl});
    bool          created = createArrayIndex(coll, "phones"_sl, R"([".type"])", "contacts[].phones", ERROR_INFO());
    REQUIRE(created);

    c4::ref query = c4query_new2(
            db, kC4N1QLQuery,
            "SELECT prof.pid, c.address.city, c.address.state, p.type, p.numbers FROM profiles AS prof UNNEST prof.contacts AS c UNNEST c.phones AS p WHERE c.type IS valued ORDER BY prof.pid"_sl,
            nullptr, ERROR_INFO());
    REQUIRE(query);

    c4::ref queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    REQUIRE(c4queryenum_getRowCount(queryenum, nullptr) == 0);

    importJSONDocs(coll, {p0001, p0002, p0003});
    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA", "home", ["310-8268551", "310-7618427"]])",
                                     R"(["p-0001", "San Pedro", "CA", "mobile", ["310-9601308"]])",
                                     R"(["p-0001", "San Pedro", "CA", "home", ["310-6653153"]])",
                                     R"(["p-0001", "San Pedro", "CA", "mobile", ["310-4833623"]])",
                                     R"(["p-0002", "Burns", "KS", "home", ["316-2417120", "316-2767391"]])",
                                     R"(["p-0002", "Burns", "KS", "mobile", ["316-8833161"]])",
                                     R"(["p-0002", "Burns", "KS", "home", ["316-9487549"]])",
                                     R"(["p-0002", "Burns", "KS", "mobile", ["316-4737548"]])",
                                     R"(["p-0003", "Tacoma", "WA", "home", ["253-0405964"]])",
                                     R"(["p-0003", "Tacoma", "WA", "mobile", ["253-7421842"]])",
                                     R"(["p-0003", "Tacoma", "WA", "home", ["253-5727806"]])",
                                     R"(["p-0003", "Tacoma", "WA", "mobile", ["253-8671217"]])",
                             });

    importJSONDocs(coll, {p0004}, 4);
    deleteDoc(coll, "0000003"_sl);
    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA", "home", ["310-8268551", "310-7618427"]])",
                                     R"(["p-0001", "San Pedro", "CA", "mobile", ["310-9601308"]])",
                                     R"(["p-0001", "San Pedro", "CA", "home", ["310-6653153"]])",
                                     R"(["p-0001", "San Pedro", "CA", "mobile", ["310-4833623"]])",
                                     R"(["p-0002", "Burns", "KS", "home", ["316-2417120", "316-2767391"]])",
                                     R"(["p-0002", "Burns", "KS", "mobile", ["316-8833161"]])",
                                     R"(["p-0002", "Burns", "KS", "home", ["316-9487549"]])",
                                     R"(["p-0002", "Burns", "KS", "mobile", ["316-4737548"]])",
                                     R"(["p-0004", "Poughkeepsie", "AR", "home", []])",
                                     R"(["p-0004", "Poughkeepsie", "AR", "mobile", ["870-5974023"]])",
                                     R"(["p-0004", "Poughkeepsie", "AR", "home", ["870-4182309"]])",
                                     R"(["p-0004", "Poughkeepsie", "AR", "mobile", ["870-1205865"]])",
                             });

    // p0001 with added work phone numbers
    constexpr std::string_view p0001_update =
            R"({"pid":"p-0001","name":{"first":"Lue","last":"Laserna"},"contacts":[{"address":{"city":"San Pedro","state":"CA","street":"19 Deer Loop","zip":"90732"},"emails":["lue.laserna@nosql-matters.org","laserna@nosql-matters.org"],"phones":[{"numbers":["310-8268551","310-7618427"],"preferred":false,"type":"home"},{"numbers":["310-9601308"],"preferred":true,"type":"mobile"},{"numbers":["310-8165215"],"preferred":false,"type":"work"}],"type":"primary"},{"address":{"city":"San Pedro","state":"CA","street":"1820 Maple Ln","zip":"90732"},"emails":["Lue@email.com","Laserna@email.com"],"phones":[{"numbers":["310-6653153"],"preferred":false,"type":"home"},{"numbers":["310-4833623"],"preferred":true,"type":"mobile"},{"numbers":["310-1548946"],"preferred":false,"type":"work"}],"type":"secondary"}],"likes":["chatting"]})";
    // p0002 with mobile phone numbers removed
    constexpr std::string_view p0002_update =
            R"({"pid":"p-0002","name":{"first":"Jasper","last":"Grebel"},"contacts":[{"address":{"city":"Burns","state":"KS","street":"19 Florida Loop","zip":"66840"},"emails":["jasper.grebel@nosql-matters.org"],"phones":[{"numbers":["316-2417120","316-2767391"],"preferred":false,"type":"home"}],"type":"primary"},{"address":{"city":"Burns","state":"KS","street":"4795 Willow Loop","zip":"66840"},"emails":["Jasper@email.com","Grebel@email.com"],"phones":[{"numbers":["316-9487549"],"preferred":true,"type":"home"}],"type":"secondary"}],"likes":["shopping"]})";

    updateDoc(coll, "0000001", p0001_update);
    updateDoc(coll, "0000002", p0002_update);
    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA", "home", ["310-8268551", "310-7618427"]])",
                                     R"(["p-0001", "San Pedro", "CA", "mobile", ["310-9601308"]])",
                                     R"(["p-0001", "San Pedro", "CA", "work", ["310-8165215"]])",
                                     R"(["p-0001", "San Pedro", "CA", "home", ["310-6653153"]])",
                                     R"(["p-0001", "San Pedro", "CA", "mobile", ["310-4833623"]])",
                                     R"(["p-0001", "San Pedro", "CA", "work", ["310-1548946"]])",
                                     R"(["p-0002", "Burns", "KS", "home", ["316-2417120", "316-2767391"]])",
                                     R"(["p-0002", "Burns", "KS", "home", ["316-9487549"]])",
                                     R"(["p-0004", "Poughkeepsie", "AR", "home", []])",
                                     R"(["p-0004", "Poughkeepsie", "AR", "mobile", ["870-5974023"]])",
                                     R"(["p-0004", "Poughkeepsie", "AR", "home", ["870-4182309"]])",
                                     R"(["p-0004", "Poughkeepsie", "AR", "mobile", ["870-1205865"]])",
                             });

    bool deleted = c4coll_deleteIndex(coll, "contacts"_sl, ERROR_INFO());
    REQUIRE(deleted);
    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA", "home", ["310-8268551", "310-7618427"]])",
                                     R"(["p-0001", "San Pedro", "CA", "mobile", ["310-9601308"]])",
                                     R"(["p-0001", "San Pedro", "CA", "work", ["310-8165215"]])",
                                     R"(["p-0001", "San Pedro", "CA", "home", ["310-6653153"]])",
                                     R"(["p-0001", "San Pedro", "CA", "mobile", ["310-4833623"]])",
                                     R"(["p-0001", "San Pedro", "CA", "work", ["310-1548946"]])",
                                     R"(["p-0002", "Burns", "KS", "home", ["316-2417120", "316-2767391"]])",
                                     R"(["p-0002", "Burns", "KS", "home", ["316-9487549"]])",
                                     R"(["p-0004", "Poughkeepsie", "AR", "home", []])",
                                     R"(["p-0004", "Poughkeepsie", "AR", "mobile", ["870-5974023"]])",
                                     R"(["p-0004", "Poughkeepsie", "AR", "home", ["870-4182309"]])",
                                     R"(["p-0004", "Poughkeepsie", "AR", "mobile", ["870-1205865"]])",
                             });
}

// 5. TestCreateAndDeleteArrayIndexesWithSharedPath
TEST_CASE_METHOD(ArrayIndexTest, "CRUD Array Index Shared Path", "[C][ArrayIndex]") {
    C4Collection* coll    = createCollection(db, {"profiles"_sl, "_default"_sl});
    bool          created = createArrayIndex(coll, "contacts"_sl, R"([".address.state"])", "contacts", ERROR_INFO());
    REQUIRE(created);
    created = createArrayIndex(coll, "phones"_sl, R"([".type"])", "contacts[].phones", ERROR_INFO());
    REQUIRE(created);

    importJSONDocs(coll, {p0001, p0002, p0003});

    c4::ref cityQuery = c4query_new2(
            db, kC4N1QLQuery,
            R"(SELECT p.pid, c.address.city, c.address.state FROM profiles AS p UNNEST p.contacts AS c WHERE c.address.state = "CA")"_sl,
            nullptr, ERROR_INFO());
    REQUIRE(cityQuery);

    c4::ref phoneQuery = c4query_new2(
            db, kC4N1QLQuery,
            R"(SELECT prof.pid, c.address.city, c.address.state, p.type, p.numbers FROM profiles AS prof UNNEST prof.contacts AS c UNNEST c.phones AS p WHERE p.type = "mobile")"_sl,
            nullptr, ERROR_INFO());
    REQUIRE(phoneQuery);

    c4::ref queryenum = REQUIRED(c4query_run(cityQuery, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0001", "San Pedro", "CA"])",
                             });
    queryenum = REQUIRED(c4query_run(phoneQuery, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA", "mobile", ["310-9601308"]])",
                                     R"(["p-0001", "San Pedro", "CA", "mobile", ["310-4833623"]])",
                                     R"(["p-0002", "Burns", "KS", "mobile", ["316-8833161"]])",
                                     R"(["p-0002", "Burns", "KS", "mobile", ["316-4737548"]])",
                                     R"(["p-0003", "Tacoma", "WA", "mobile", ["253-7421842"]])",
                                     R"(["p-0003", "Tacoma", "WA", "mobile", ["253-8671217"]])",
                             });

    bool deleted = c4coll_deleteIndex(coll, "phones"_sl, ERROR_INFO());
    REQUIRE(deleted);

    // cityQuery is not affected by the deletion of index "phones"
    queryenum = REQUIRED(c4query_run(cityQuery, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0001", "San Pedro", "CA"])",
                             });

    // phoneQuery is affected by the deletion of index "phones"
    // Following error will be logged,
    // 2024-10-29T21:14:28.226339 DB ERROR SQLite error (code 1): no such table: 152b9815998e188eb99eb1612aafbb3ee6031535 in "SELECT fl_result(fl_value(prof.body, 'pid')), fl_result(fl_unnested_value(c.body, 'address.city')), fl_result(fl_unnested_value(c.body, 'address.state')), fl_result(fl_unnested_value(p.body, 'type')), fl_result(fl_unnested_value(p.body, 'numbers')) FROM "kv_.profiles" AS prof JOIN bc89db8a20fe759bf161b84adf2294d9bfe0c88d AS c ON c.docid=prof.rowid JOIN "152b9815998e188eb99eb1612aafbb3ee6031535" AS p ON p.docid=c.rowid WHERE fl_unnested_value(p.body, 'type') = 'mobile'". This table is referenced by an array index, which may have been deleted.
    C4Error error;
    queryenum = c4query_run(phoneQuery, nullslice, &error);
    CHECK(!queryenum);  // This query relies on the index that has been deleted.
    CHECK((error.domain == SQLiteDomain && error.code == 1));

    // Recompile the query
    phoneQuery = c4query_new2(
            db, kC4N1QLQuery,
            R"(SELECT prof.pid, c.address.city, c.address.state, p.type, p.numbers FROM profiles AS prof UNNEST prof.contacts AS c UNNEST c.phones AS p WHERE p.type = "mobile")"_sl,
            nullptr, ERROR_INFO());
    REQUIRE(phoneQuery);
    queryenum = c4query_run(phoneQuery, nullslice, &error);
    CHECK(queryenum);

    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA", "mobile", ["310-9601308"]])",
                                     R"(["p-0001", "San Pedro", "CA", "mobile", ["310-4833623"]])",
                                     R"(["p-0002", "Burns", "KS", "mobile", ["316-8833161"]])",
                                     R"(["p-0002", "Burns", "KS", "mobile", ["316-4737548"]])",
                                     R"(["p-0003", "Tacoma", "WA", "mobile", ["253-7421842"]])",
                                     R"(["p-0003", "Tacoma", "WA", "mobile", ["253-8671217"]])",
                             });
}

// 6. TestArrayIndexEmptyArray
TEST_CASE_METHOD(ArrayIndexTest, "Array Index Empty Array", "[C][ArrayIndex]") {
    C4Collection* coll = createCollection(db, {"profiles"_sl, "_default"_sl});

    // p0001 with empty contacts array
    constexpr std::string_view p0001_empty =
            R"({"pid":"p-0001","name":{"first":"Lue","last":"Laserna"},"contacts":[],"likes":["chatting"]})";
    importJSONDocs(coll, {p0001});
    updateDoc(coll, "0000001", p0001_empty);

    c4::ref query = c4query_new2(
            db, kC4N1QLQuery,
            R"(SELECT p.pid, c.address.city, c.address.state FROM profiles AS p UNNEST p.contacts AS c WHERE c.address.state = "CA")"_sl,
            nullptr, ERROR_INFO());
    REQUIRE(query);

    c4::ref queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {});

    bool created = createArrayIndex(coll, "contacts"_sl, R"([".address.state"])", "contacts", ERROR_INFO());
    REQUIRE(created);

    query = c4query_new2(
            db, kC4N1QLQuery,
            R"(SELECT p.pid, c.address.city, c.address.state FROM profiles AS p UNNEST p.contacts AS c WHERE c.address.state = "CA")"_sl,
            nullptr, ERROR_INFO());
    REQUIRE(query);

    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {});
}

// 7. TestArrayIndexMissingArray
TEST_CASE_METHOD(ArrayIndexTest, "Array Index Missing Array", "[C][ArrayIndex]") {
    C4Collection* coll = createCollection(db, {"profiles"_sl, "_default"_sl});

    // p0001 with missing contacts array
    constexpr std::string_view p0001_missing =
            R"({"pid":"p-0001","name":{"first":"Lue","last":"Laserna"},"likes":["chatting"]})";
    importJSONDocs(coll, {p0001});
    updateDoc(coll, "0000001", p0001_missing);

    c4::ref query =
            c4query_new2(db, kC4N1QLQuery,
                         "SELECT p.pid, c.address.city, c.address.state FROM profiles AS p UNNEST p.contacts AS c"_sl,
                         nullptr, ERROR_INFO());
    REQUIRE(query);

    c4::ref queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {});

    bool created = createArrayIndex(coll, "contacts"_sl, R"([".address.state"])", "contacts", ERROR_INFO());
    REQUIRE(created);

    query = c4query_new2(db, kC4N1QLQuery,
                         "SELECT p.pid, c.address.city, c.address.state FROM profiles AS p UNNEST p.contacts AS c"_sl,
                         nullptr, ERROR_INFO());
    REQUIRE(query);

    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {});
}

// 8. TestArrayIndexNonArray
TEST_CASE_METHOD(ArrayIndexTest, "Array Index Non-Array", "[C][ArrayIndex]") {
    C4Collection* coll = createCollection(db, {"profiles"_sl, "_default"_sl});

    // p0001 with 'contacts' scalar instead of array
    constexpr std::string_view p0001_scalar =
            R"({"pid":"p-0001","name":{"first":"Lue","last":"Laserna"},"contacts":"foo","likes":["chatting"]})";
    importJSONDocs(coll, {p0001});
    updateDoc(coll, "0000001", p0001_scalar);

    c4::ref query =
            c4query_new2(db, kC4N1QLQuery,
                         "SELECT p.pid, c.address.city, c.address.state FROM profiles AS p UNNEST p.contacts AS c"_sl,
                         nullptr, ERROR_INFO());
    REQUIRE(query);

    c4::ref queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {R"(["p-0001", null, null])"});

    bool created = createArrayIndex(coll, "contacts"_sl, R"([".address.state"])", "contacts", ERROR_INFO());
    REQUIRE(created);

    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {R"(["p-0001", null, null])"});
}

// - UNNEST
// 1. TestUnnestSingleLevelScalar
TEST_CASE_METHOD(ArrayIndexTest, "Unnest Single Level Scalar", "[C][Unnest]") {
    C4Collection* coll = createCollection(db, {"profiles"_sl, "_default"_sl});
    importTestData(coll);
    c4::ref query = c4query_new2(
            db, kC4N1QLQuery,
            R"(SELECT p.pid, likes FROM profiles as p UNNEST p.likes AS likes WHERE likes = "travelling" ORDER BY p.pid LIMIT 5)"_sl,
            nullptr, ERROR_INFO());
    c4::ref queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));

    validateQuery(queryenum, {
                                     R"(["p-0010", "travelling"])",
                                     R"(["p-0027", "travelling"])",
                                     R"(["p-0037", "travelling"])",
                                     R"(["p-0060", "travelling"])",
                                     R"(["p-0068", "travelling"])",
                             });
}

// 2. TestUnnestSingleLevelNonScalar
TEST_CASE_METHOD(ArrayIndexTest, "Unnest Single Level Non-Scalar", "[C][Unnest]") {
    C4Collection* coll = createCollection(db, {"profiles"_sl, "_default"_sl});
    importTestData(coll);
    c4::ref query = c4query_new2(
            db, kC4N1QLQuery,
            R"(SELECT p.pid, c.address.city, c.address.state FROM profiles AS p UNNEST p.contacts AS c WHERE c.address.state = "CA" ORDER BY p.pid, c.address.city LIMIT 5)"_sl,
            nullptr, ERROR_INFO());

    bool created = createArrayIndex(coll, "contacts"_sl, R"([])"_sl, "contacts", ERROR_INFO());
    REQUIRE(created);

    c4::ref queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0015", "Santa Cruz", "CA"])",
                                     R"(["p-0015", "Santa Cruz", "CA"])",
                                     R"(["p-0036", "Pasadena", "CA"])",
                             });

    bool deleted = c4coll_deleteIndex(coll, "contacts"_sl, ERROR_INFO());
    REQUIRE(deleted);
    created = createArrayIndex(coll, "contacts"_sl, R"([".address.state"])"_sl, "contacts", ERROR_INFO());
    REQUIRE(created);

    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0015", "Santa Cruz", "CA"])",
                                     R"(["p-0015", "Santa Cruz", "CA"])",
                                     R"(["p-0036", "Pasadena", "CA"])",
                             });
}

// 3. TestUnnestNestedScalarArray
TEST_CASE_METHOD(ArrayIndexTest, "Unnest Nested Scalar Array", "[C][Unnest]") {
    C4Collection* coll = createCollection(db, {"profiles"_sl, "_default"_sl});
    importTestData(coll);

    c4::ref query = c4query_new2(
            db, kC4N1QLQuery,
            R"(SELECT p.pid, email FROM profiles AS p UNNEST p.contacts AS c UNNEST c.emails as email ORDER BY p.pid, email LIMIT 5)"_sl,
            nullptr, ERROR_INFO());

    c4::ref queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "Laserna@email.com"])",
                                     R"(["p-0001", "Lue@email.com"])",
                                     R"(["p-0001", "laserna@nosql-matters.org"])",
                                     R"(["p-0001", "lue.laserna@nosql-matters.org"])",
                                     R"(["p-0002", "Grebel@email.com"])",
                             });

    bool created = createArrayIndex(coll, "emails"_sl, R"([])"_sl, "contacts[].emails", ERROR_INFO());
    REQUIRE(created);

    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "Laserna@email.com"])",
                                     R"(["p-0001", "Lue@email.com"])",
                                     R"(["p-0001", "laserna@nosql-matters.org"])",
                                     R"(["p-0001", "lue.laserna@nosql-matters.org"])",
                                     R"(["p-0002", "Grebel@email.com"])",
                             });
}

// 4. TestUnnestNestedNonScalarArray
TEST_CASE_METHOD(ArrayIndexTest, "Unnest Nested Non-Scalar Array", "[C][Unnest]") {
    C4Collection* coll = createCollection(db, {"profiles"_sl, "_default"_sl});
    importTestData(coll);

    c4::ref query = c4query_new2(
            db, kC4N1QLQuery,
            R"(SELECT pr.pid, c.address.city, c.address.state, ph.type, ph.preferred, ph.numbers FROM profiles as pr UNNEST pr.contacts AS c UNNEST c.phones AS ph WHERE ph.preferred = true ORDER BY pr.pid, c.city, ph.type LIMIT 5)"_sl,
            nullptr, ERROR_INFO());

    bool created = createArrayIndex(coll, "phones"_sl, R"([".preferred"])"_sl, "contacts[].phones", ERROR_INFO());
    REQUIRE(created);

    c4::ref queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA", "mobile", true, ["310-9601308"]])",
                                     R"(["p-0001", "San Pedro", "CA", "mobile", true, ["310-4833623"]])",
                                     R"(["p-0002", "Burns", "KS", "home", true, ["316-9487549"]])",
                                     R"(["p-0002", "Burns", "KS", "mobile", true, ["316-8833161"]])",
                                     R"(["p-0003", "Tacoma", "WA", "mobile", true, ["253-7421842"]])",
                             });

    created = createArrayIndex(coll, "phones"_sl, R"([".type", ".preferred"])"_sl, "contacts[].phones", ERROR_INFO());
    REQUIRE(created);

    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA", "mobile", true, ["310-9601308"]])",
                                     R"(["p-0001", "San Pedro", "CA", "mobile", true, ["310-4833623"]])",
                                     R"(["p-0002", "Burns", "KS", "home", true, ["316-9487549"]])",
                                     R"(["p-0002", "Burns", "KS", "mobile", true, ["316-8833161"]])",
                                     R"(["p-0003", "Tacoma", "WA", "mobile", true, ["253-7421842"]])",
                             });
}

// 5. TestUnnestSingleLevelArrayWithGroupBy
// Disabled until group-by is fixed
// See https://jira.issues.couchbase.com/browse/CBL-6327
#if 0
TEST_CASE_METHOD(ArrayIndexTest, "Unnest Single Level Array With Group By", "[C][Unnest]") {
    C4Collection* coll = createCollection(db, {"profiles"_sl, "_default"_sl});
    importTestData(coll);

    c4::ref query = c4query_new2(db, kC4N1QLQuery, "SELECT likes, count(1) FROM profiles AS p UNNEST p.likes AS likes LIMIT 10 ORDER BY likes"_sl, nullptr, ERROR_INFO());
    REQUIRE(query);

    c4::ref queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {});
}
#endif

// 6. TestUnnestWithoutAlias
TEST_CASE_METHOD(ArrayIndexTest, "Unnest Without Alias", "[C][Unnest]") {
    C4Collection* coll = createCollection(db, {"profiles"_sl, "_default"_sl});
    importTestData(coll);

    c4::ref query = c4query_new2(
            db, kC4N1QLQuery,
            R"(SELECT profiles.pid, contacts.address.city, contacts.address.state FROM profiles UNNEST profiles.contacts WHERE contacts.address.state = "CA" ORDER BY profiles.pid, contacts.address.city LIMIT 5)"_sl,
            nullptr, ERROR_INFO());
    REQUIRE(query);

    bool created = createArrayIndex(coll, "contacts"_sl, "[]"_sl, "contacts", ERROR_INFO());
    REQUIRE(created);

    c4::ref queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0015", "Santa Cruz", "CA"])",
                                     R"(["p-0015", "Santa Cruz", "CA"])",
                                     R"(["p-0036", "Pasadena", "CA"])",
                             });

    created = createArrayIndex(coll, "contacts"_sl, R"([".address.state"])"_sl, "contacts", ERROR_INFO());
    REQUIRE(created);

    queryenum = REQUIRED(c4query_run(query, nullslice, nullptr));
    validateQuery(queryenum, {
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0001", "San Pedro", "CA"])",
                                     R"(["p-0015", "Santa Cruz", "CA"])",
                                     R"(["p-0015", "Santa Cruz", "CA"])",
                                     R"(["p-0036", "Pasadena", "CA"])",
                             });
}

// 7. TestUnnestArrayLiteralNotSupport
TEST_CASE_METHOD(ArrayIndexTest, "Unnest Array Literal Not Supported", "[C][Unnest]") {
    C4Collection* coll = createCollection(db, {"profiles"_sl, "_default"_sl});
    importTestData(coll);

    C4Error err{};
    c4::ref query = c4query_new2(
            db, kC4N1QLQuery,
            R"(SELECT p.pid, c.address.city, c.address.state FROM profiles AS p UNNEST ["a", "b", "c"])"_sl, nullptr,
            &err);
    REQUIRE(!query);
    CHECK(err.code == kC4ErrorInvalidQuery);

    query = c4query_new2(
            db, kC4N1QLQuery,
            R"(SELECT p.pid, c.address.city, c.address.state, ph.type, ph.preferred, ph.numbers FROM profiles AS p UNNEST p.contacts AS c UNNEST ["a", "b", "c"] AS ph)"_sl,
            nullptr, &err);
    REQUIRE(!query);
    CHECK(err.code == kC4ErrorInvalidQuery);
}
