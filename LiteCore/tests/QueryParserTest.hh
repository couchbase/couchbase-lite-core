//
//  QueryParserTest.hh
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
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
    virtual string collectionTableName(const string &collection, DeletionStatus type) const override {
        CHECK(!hasPrefix(collection, "kv_"));   // make sure I didn't get passed a table name
        string table;
        if (type == kLiveAndDeletedDocs) {
            table = "all_";
        } else {
            table = "kv_";
            if (type == kDeletedDocs)
                table += "del_";
        }
        if (collection == "_default")
            table += "default";
        else
            table += "coll_" + collection;
        return table;
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
