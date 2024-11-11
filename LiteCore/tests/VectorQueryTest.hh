//
// VectorQueryTest.hh
//
// Copyright © 2024 Couchbase. All rights reserved.
//

#pragma once
#include "QueryTest.hh"
#include "SQLiteDataFile.hh"
#include "Base64.hh"
#include <map>
#include <mutex>

class VectorQueryTest : public QueryTest {
  public:
    static int initialize() {
        // Note: The extension path has to be set before the DataFile is opened,
        // so it will load the extension. That's why this is called in a param of the parent ctor.
        std::once_flag sOnce;
        std::call_once(sOnce, [] {
            if ( const char* path = getenv("LiteCoreExtensionPath") ) {
                sExtensionPath = path;
                litecore::SQLiteDataFile::enableExtension("CouchbaseLiteVectorSearch", sExtensionPath);
                Log("Registered LiteCore extension path %s", path);
            }
        });
        return 0;
    }

    explicit VectorQueryTest(int which) : QueryTest(which + initialize()) {}

    ~VectorQueryTest() {
        // Assert that the callback did not log unexpected warnings:
        CHECK(warningsLogged() == expectedWarningsLogged);
    }

    void requireExtensionAvailable() {
        if ( sExtensionPath.empty() )
            FAIL("You must setenv LiteCoreExtensionPath, to the directory containing the "
                 "CouchbaseLiteVectorSearch extension");
    }

    void createVectorIndex(string const& name, string const& expression, IndexSpec::VectorOptions const& options,
                           QueryLanguage lang = QueryLanguage::kJSON) {
        requireExtensionAvailable();
        if ( lang == QueryLanguage::kJSON ) {
            IndexSpec spec(name, IndexSpec::kVector, alloc_slice(json5(expression)), QueryLanguage::kJSON, options);
            store->createIndex(spec);
        } else {
            IndexSpec spec(name, IndexSpec::kVector, alloc_slice(expression), QueryLanguage::kN1QL, options);
            store->createIndex(spec);
        }
        REQUIRE(store->getIndexes().size() == 1);
    }

    Retained<Doc> inspectVectorIndex(string const& name) {
        int64_t     rowCount;
        alloc_slice rowData;
        dynamic_cast<SQLiteDataFile&>(store->dataFile()).inspectIndex(name, rowCount, &rowData);
        Log("Index has %" PRIi64 " rows", rowCount);
        return make_retained<Doc>(rowData);
    }

    Query::Options optionsWithTargetVector(const float target[128], valueType asType) {
        Encoder enc;
        enc.beginDictionary();
        enc.writeKey("target");
        switch ( asType ) {
            case kString:
                enc.writeString(base64::encode(slice(target, 128 * sizeof(float))));
                break;
            case kData:
                enc.writeData(slice(target, 128 * sizeof(float)));
                break;
            case kArray:
                enc.beginArray();
                for ( size_t i = 0; i < 128; ++i ) enc.writeFloat(target[i]);
                enc.endArray();
                break;
            default:
                FAIL("unsupported type for vector");
        }
        enc.endDictionary();
        return Query::Options(enc.finish());
    }

    void checkExpectedResults(Retained<QueryEnumerator> e, std::initializer_list<slice> expectedIDs,
                              std::initializer_list<float> expectedDistances) {
        auto expectedID   = expectedIDs.begin();
        auto expectedDist = expectedDistances.begin();
        for ( size_t i = 0; i < expectedIDs.size(); ++i, expectedID++, expectedDist++ ) {
            INFO("i=" << i);
            REQUIRE(e->next());
            slice id       = e->columns()[0]->asString();
            float distance = e->columns()[1]->asFloat();
            CHECK(id == *expectedID);
            CHECK_distances(distance, *expectedDist);
        }
        CHECK(!e->next());
    }

    // Vector encoders are lossy, so using one in the index will result in approximate distances,
    // which is why the distance check below is so loose.
    static void CHECK_distances(float distance, float expectedDist) {
        CHECK_THAT(distance,
                   Catch::Matchers::WithinRel(expectedDist, 0.20f) || Catch::Matchers::WithinAbs(expectedDist, 400.0f));
    }

    /// Increment this if the test is expected to generate a warning.
    unsigned expectedWarningsLogged = 0;

    static inline string sExtensionPath;
};
