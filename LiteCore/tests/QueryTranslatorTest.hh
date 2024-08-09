//
// QueryTranslatorTest.hh
//
// Copyright © 2024 Couchbase. All rights reserved.
//

#pragma once
#include "LiteCoreTest.hh"
#include "fleece/Fleece.h"
#include <set>

namespace litecore::qt {
    class SourceNode;
}

struct QueryTranslatorTest : public TestFixture {
    string               parseWhere(string_view json);
    string               parse(string_view json);
    string               parse(FLValue);
    void                 mustFail(string_view json);
    void                 fillInTableName(litecore::qt::SourceNode* source);
    [[nodiscard]] string collectionTableName(litecore::qt::SourceNode const& source) const;
    [[nodiscard]] string unnestedTableName(const string& onTable, const string& property) const;
    [[nodiscard]] string vectorTableName(const string& onTable, const std::string& property,
                                         string_view metricName) const;
    [[nodiscard]] bool   tableExists(string tableName) const;
    void                 CHECK_equal(string_view result, string_view expected);

    std::set<string> tableNames{"kv_default", "kv_del_default"};
    std::set<string> usedTableNames;
    std::map<std::pair<string, string>, string>
                vectorIndexedProperties;  // maps {table name,expression JSON} -> vector-index table name
    std::string vectorIndexMetric;
};
