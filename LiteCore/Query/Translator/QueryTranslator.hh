//
// QueryTranslator.hh
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Base.hh"
#include "fleece/function_ref.hh"
#include "fleece/Fleece.h"
#include <set>
#include <vector>

namespace litecore {
    namespace qt {
        class Node;
        class QueryNode;
        class SQLWriter;
    }  // namespace qt

    /** Translates queries from our JSON schema (actually Fleece) into SQL runnable by SQLite.
        https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
        For some architectural info, see docs/QueryTranslator.md */
    class QueryTranslator {
      public:
        /** Which docs to include from a collection in a query; determines which table to use. */
        enum DeletionStatus { kLiveDocs, kDeletedDocs, kLiveAndDeletedDocs };

        /** Delegate knows about the naming & existence of tables.
            Implemented by SQLiteDataFile; this interface is to keep the QueryTranslator isolated from
            such details and make it easier to unit-test. */
        class Delegate {
          public:
            using DeletionStatus                                                    = QueryTranslator::DeletionStatus;
            virtual ~Delegate()                                                     = default;
            [[nodiscard]] virtual bool   tableExists(const string& tableName) const = 0;
            [[nodiscard]] virtual string collectionTableName(const string& collection, DeletionStatus) const    = 0;
            [[nodiscard]] virtual string FTSTableName(const string& onTable, const string& property) const      = 0;
            [[nodiscard]] virtual string unnestedTableName(const string& onTable, const string& property) const = 0;
#ifdef COUCHBASE_ENTERPRISE
            [[nodiscard]] virtual string predictiveTableName(const string& onTable, const string& property) const = 0;
            [[nodiscard]] virtual string vectorTableName(const string& collection, const std::string& property,
                                                         string_view metricName) const                            = 0;
#endif
        };

        QueryTranslator(const Delegate& delegate, string defaultCollectionName, string defaultTableName);
        ~QueryTranslator();

        /// Parses a query in Fleece parsed from JSON.
        void parse(FLValue);

        /// Parses a query in JSON format
        void parseJSON(slice);

        /// The translated SQLite-flavor SQL.
        string const& SQL() const { return _sql; }

        const std::set<string>& parameters() { return _parameters; }

        const std::set<string>& collectionTablesUsed() const { return _kvTables; }

        const std::vector<string>& ftsTablesUsed() const { return _ftsTables; }

        unsigned firstCustomResultColumn() const { return _1stCustomResultCol; }

        const std::vector<string>& columnTitles() const { return _columnTitles; }

        bool isAggregateQuery() const { return _isAggregateQuery; }

        bool usesExpiration() const { return _usesExpiration; }

        /// Translates an expression to SQL and returns it directly.
        string expressionSQL(FLValue);

        //======== INDEX CREATION:

        /// Renames the `body` column; used by index creation code when defining triggers.
        void setBodyColumnName(string name) { _bodyColumnName = name; }

        /// Writes a CREATE INDEX statement.
        void writeCreateIndex(const string& indexName, const string& onTableName, FLArrayIterator& whatExpressions,
                              FLArray whereClause, bool isUnnestedTable);

        /// Returns a WHERE clause.
        /// @param  expr  The parsed JSON expression
        /// @param dbAlias  The table alias to use
        string whereClauseSQL(FLValue expr, string_view dbAlias);

        /// Translates the JSON-parsed Value to a SQL expression for use in a FTS index.
        string FTSExpressionSQL(FLValue);

        /// Returns the column name of an FTS table to use for a MATCH expression.
        static string FTSColumnName(FLValue expression);

        /// Translates the JSON-parsed expression into a SQL string that evaluates to the vector
        /// value of that expression, or NULL. Used by SQLiteKeyStore::createVectorIndex.
        string vectorToIndexExpressionSQL(FLValue exprToIndex, unsigned dimensions);

        string eachExpressionSQL(FLValue);
        string unnestedTableName(FLValue key) const;
        string predictiveTableName(FLValue) const;

      private:
        QueryTranslator(const QueryTranslator& qp)         = delete;
        QueryTranslator& operator=(const QueryTranslator&) = delete;
        string           writeSQL(function_ref<void(qt::SQLWriter&)>);
        string           functionCallSQL(slice fnName, FLValue arg, FLValue param = nullptr);

        const Delegate&     _delegate;                 // delegate object (SQLiteKeyStore)
        string              _defaultTableName;         // Name of the default table to use
        string              _defaultCollectionName;    // Name of the default collection to use
        string              _sql;                      // The generated SQL
        std::set<string>    _parameters;               // Plug-in "$" parameters found in parsing
        std::set<string>    _kvTables;                 // Collection tables referenced in this query
        std::vector<string> _ftsTables;                // FTS virtual tables being used
        unsigned            _1stCustomResultCol{0};    // Index of 1st result after _baseResultColumns
        std::vector<string> _columnTitles;             // Pretty names of result columns
        bool                _isAggregateQuery{false};  // Is this an aggregate query?
        bool                _usesExpiration{false};    // Has query accessed _expiration meta-property?

        string _bodyColumnName;
    };

}  // namespace litecore
