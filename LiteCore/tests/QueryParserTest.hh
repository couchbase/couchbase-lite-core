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
#include "fleece/Fleece.h"
#include <string>
#include <set>
#include "LiteCoreTest.hh"


class QueryParserTest : public TestFixture, protected QueryParser::Delegate {
public:
    QueryParserTest() =default;

    string parse(FLValue val);
    string parse(string json);
    string parseWhere(string json);
    void mustFail(string json);

protected:
    virtual string collectionTableName(const string &collection) const override {
        CHECK(!hasPrefix(collection, "kv_"));   // make sure I didn't get passed a table name
        if (collection == "_default" || collection == "_")
            return "kv_default";
        else
            return "kv_coll_" + collection;
    }
    virtual std::string FTSTableName(const string &onTable, const std::string &property) const override {
        return onTable + "::" + property;
    }
    virtual std::string unnestedTableName(const string &onTable, const std::string &property) const override {
        return onTable + ":unnest:" + property;
    }
    virtual bool tableExists(const string &tableName) const override {
        return tableNames.count(tableName) > 0;
    }
#ifdef COUCHBASE_ENTERPRISE
    virtual std::string predictiveTableName(const string &onTable, const std::string &property) const override {
        return onTable + ":predict:" + property;
    }
#endif

    std::set<string> tableNames {"kv_default"};

    std::set<string> usedTableNames;
};
