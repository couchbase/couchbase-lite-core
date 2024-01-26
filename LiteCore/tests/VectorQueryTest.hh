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
        // Assert that the callback did not log a warning:
        CHECK(warningsLogged() == 0);
    }

    void requireExtensionAvailable() {
        if ( sExtensionPath.empty() )
            FAIL("You must setenv LiteCoreExtensionPath, to the directory containing the "
                 "CouchbaseLiteVectorSearch extension");
    }

    void createVectorIndex(string const& name, string const& expressionJSON, IndexSpec::VectorOptions const& options) {
        requireExtensionAvailable();
        IndexSpec spec(name, IndexSpec::kVector, alloc_slice(json5(expressionJSON)), QueryLanguage::kJSON, options);
        store->createIndex(spec);
        REQUIRE(store->getIndexes().size() == 1);
    }

    static inline string sExtensionPath;
};
