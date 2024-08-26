//
//  QueryParserTest.hh
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "QueryParser.hh"
#include "StringUtil.hh"
#include "fleece/FLValue.h"
#include "fleece/FLJSON.h"
#include <string>
#include <set>
#include "LiteCoreTest.hh"

class QueryParserTest
    : public TestFixture
    , protected QueryParser::Delegate {
  public:
    QueryParserTest() = default;

    string parse(FLValue val);
    string parse(const string& json);
    string parseWhere(const string& json);
    void   mustFail(const string& json);

  protected:
    [[nodiscard]] string collectionTableName(const string& collection, DeletionStatus type) const override {
        // This is a simplified version of SQLiteDataFile::collectionTableName()
        CHECK(!hasPrefix(collection, "kv_"));  // make sure I didn't get passed a table name
        string table;
        if ( type == litecore::QueryParser::kLiveAndDeletedDocs ) {
            table = "all_";
        } else {
            table = "kv_";
            if ( type == litecore::QueryParser::kDeletedDocs ) table += "del_";
        }
        if ( slice(collection) == KeyStore::kDefaultCollectionName || collection == "_" )
            table += DataFile::kDefaultKeyStoreName;
        else
            (table += KeyStore::kCollectionPrefix) += collection;
        return table;
    }

    [[nodiscard]] std::string FTSTableName(const string& onTable, const std::string& property) const override {
        return onTable + "::" + property;
    }

    [[nodiscard]] std::string unnestedTableName(const string& onTable, const std::string& property) const override {
        return onTable + ":unnest:" + property;
    }

    [[nodiscard]] bool tableExists(const string& tableName) const override {
        return ((string_view)tableName).substr(0, 4) == "all_" || tableNames.count(tableName) > 0;
    }

#ifdef COUCHBASE_ENTERPRISE
    [[nodiscard]] std::string predictiveTableName(const string& onTable, const std::string& property) const override {
        return onTable + ":predict:" + property;
    }

    [[nodiscard]] std::string vectorTableName(const string& onTable, const std::string& property,
                                              string_view metricName) const override {
        auto i = vectorIndexedProperties.find({onTable, property});
        if ( i == vectorIndexedProperties.end() )
            FAIL("there is no vector index of expression " + property + " on table " + onTable);
        string tableName = i->second;
        REQUIRE(tableExists(tableName));
        if ( !metricName.empty() ) REQUIRE(metricName == vectorIndexMetric);
        return tableName;
    }
#endif

    std::set<string> tableNames{"kv_default", "kv_del_default"};
    std::set<string> usedTableNames;
    std::map<std::pair<string, string>, string>
                vectorIndexedProperties;  // maps {table name,expression JSON} -> vector-index table name
    std::string vectorIndexMetric;
};
