//
// VectorQueryTest.hh
//
// Copyright © 2024 Couchbase. All rights reserved.
//

#pragma once
#include "QueryTest.hh"
#include "SQLiteDataFile.hh"
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
                litecore::SQLiteDataFile::setExtensionPath(sExtensionPath);
                Log("Registered LiteCore extension path %s", path);
            }
        });
        return 0;
    }

    VectorQueryTest(int which) : QueryTest(which + initialize()) {}

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

    void checkExpectedResults(Retained<QueryEnumerator> e, std::initializer_list<slice> expectedIDs,
                              std::initializer_list<float> expectedDistances) {
        auto expectedID   = expectedIDs.begin();
        auto expectedDist = expectedDistances.begin();
        for ( size_t i = 0; i < expectedIDs.size(); ++i, expectedID++, expectedDist++ ) {
            REQUIRE(e->next());
            slice id       = e->columns()[0]->asString();
            float distance = e->columns()[1]->asFloat();
            INFO("i=" << i);
            CHECK(id == *expectedID);
            // Vector encoders are lossy, so using one in the index will result in approximate distances,
            // which is why the distance check below is so loose.
            CHECK_THAT(distance, Catch::Matchers::WithinRel(*expectedDist, 0.20f)
                                         || Catch::Matchers::WithinAbs(*expectedDist, 400.0f));
        }
        CHECK(!e->next());
    }

    /// Increment this if the test is expected to generate a warning.
    unsigned expectedWarningsLogged = 0;

    static inline string sExtensionPath;
};
