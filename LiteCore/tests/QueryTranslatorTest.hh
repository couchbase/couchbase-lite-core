//
// QueryTranslatorTest.hh
//
// Copyright © 2024 Couchbase. All rights reserved.
//

#pragma once
#include "QueryTranslator.hh"
#include "LiteCoreTest.hh"
#include "fleece/Fleece.h"
#include <set>

namespace litecore::qt {
    class SourceNode;
}

struct QueryTranslatorTest
    : public TestFixture
    , public QueryTranslator::Delegate {
    string parseWhere(string_view json);
    string parse(string_view json);
    string parse(FLValue);
    void   mustFail(string_view json);
    void   CHECK_equal(string_view result, string_view expected);

    // QueryTranslator::Delegate:
    [[nodiscard]] virtual bool   tableExists(const string& tableName) const override;
    [[nodiscard]] virtual string collectionTableName(const string& collection, DeletionStatus) const override;
    [[nodiscard]] virtual string FTSTableName(const string& onTable, const string& property) const override;
    [[nodiscard]] virtual string unnestedTableName(const string& onTable, const string& property) const override;
#ifdef COUCHBASE_ENTERPRISE
    [[nodiscard]] virtual string predictiveTableName(const string& onTable, const string& property) const override;
    [[nodiscard]] virtual string vectorTableName(const string& collection, const std::string& property,
                                                 string_view metricName) const override;
#endif

    string           databaseName = "db";
    std::set<string> tableNames{"kv_default", "kv_del_default"};
    std::map<std::pair<string, string>, string>
                             vectorIndexedProperties;  // maps {table name,expression JSON} -> vector-index table name
    std::string              vectorIndexMetric;
    mutable std::set<string> usedTableNames;
};
