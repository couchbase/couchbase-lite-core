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
#include "fleece/Fleece.h"
#include <string>
#include "LiteCoreTest.hh"


class QueryParserTest : public TestFixture, protected QueryParser::delegate {
public:
    QueryParserTest() { }

    string parse(FLValue val);
    string parse(string json);
    string parseWhere(string json);
    void mustFail(string json);

protected:
    virtual std::string tableName() const override {
        return "kv_default";
    }
    virtual std::string FTSTableName(const std::string &property) const override {
        return tableName() + "::" + property;
    }
    virtual std::string unnestedTableName(const std::string &property) const override {
        return tableName() + ":unnest:" + property;
    }
    virtual bool tableExists(const string &tableName) const override {
        return tablesExist;
    }
#ifdef COUCHBASE_ENTERPRISE
    virtual std::string predictiveTableName(const std::string &property) const override {
        return tableName() + ":predict:" + property;
    }
#endif

    bool tablesExist {false};
};
