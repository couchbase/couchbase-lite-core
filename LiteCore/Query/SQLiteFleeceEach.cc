//
// SQLiteFleeceEach.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//
//
//  (Heavily) adapted from ext/misc/json1.c and ext/misc/series.c in the SQLite source code:
//  http://www.sqlite.org/src/artifact?ci=trunk&filename=ext/misc/series.c
//  http://www.sqlite.org/src/artifact?ci=trunk&filename=ext/misc/json1.c
//
//  Documentation of the json_each function that this is based on:
//  https://www.sqlite.org/json1.html#jeach
//
//  Documentation on table-valued functions: http://www.sqlite.org/vtab.html#tabfunc2
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "SQLite_Internal.hh"
#include "SQLiteFleeceUtil.hh"
#include "Path.hh"
#include <optional>

#include <sqlite3.h>

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {


    // Column numbers; these correspond to the CREATE TABLE statement below
    enum {
        kKeyColumn = 0,         // 'key':   The dictionary key (null for array items)
        kValueColumn,           // 'value': The item as a SQL value
        kTypeColumn,            // 'type':  The item's type, an integer
        kDataColumn,            // 'data':  The item as encoded Fleece data
        kBodyColumn,            // 'body':  The item as a raw Value*
        kRootFleeceDataColumn,  // 'root_data': The Fleece data of the root [hidden]
        kRootPathColumn,        // 'root_path': Path from the root to the item being iterated [hidden]
    };

    // Index used; stored in 'idxNum'
    enum {
        kNoIndex = 0,
        kFleeceDataIndex,
        kPathIndex,
    };

    // Registered virtual-table instance that hangs onto the necessary per-database context info.
    struct FleeceVTab : public sqlite3_vtab {
        fleeceFuncContext context;
    };

    // FleeceCursor is a subclass of sqlite3_vtab_cursor which will
    // serve as the underlying representation of a cursor that scans over rows of the result
    class FleeceCursor : public sqlite3_vtab_cursor {
      private:
        // Instance data:
        FleeceVTab     *_vtab;                      // The virtual table
        optional<Scope> _scope;                     // Fleece document
        bool            _scopeDataIsCopied{false};  // If true, _scope.data() is a malloc'ed block
        alloc_slice     _rootPath;                  // The path string within the data, if any
        const Value    *_container;                 // The object being iterated (target of the path)
        valueType       _containerType;             // The value type of _container
        uint32_t        _rowid;                     // The current row number, starting at 0
        uint32_t        _rowCount;                  // The number of rows


#pragma mark - STATIC METHODS (DIRECT CALLBACKS):

        // instances are allocated via malloc, i.e. no exceptions raised
        static void *operator new(size_t size) noexcept { return malloc(size); }

        static void operator delete(void *mem) noexcept { free(mem); }

        // Creates a new sqlite3_vtab that describes the virtual table.
        static int connect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **outVtab,
                           char **outErr) noexcept {
            /* "A virtual table that contains hidden columns can be used like a table-valued function
            in the FROM clause of a SELECT statement. The arguments to the table-valued function
            become constraints on the HIDDEN columns of the virtual table." */
            int rc = sqlite3_declare_vtab(db, "CREATE TABLE x(key, value, type, data, body,"
                                              " root_data HIDDEN, root_path HIDDEN)");
            if ( rc != SQLITE_OK ) return rc;

            // Allocate a new FleeceVTab and copy the context into it:
            auto vtab = (FleeceVTab *)malloc(sizeof(FleeceVTab));
            if ( !vtab ) return SQLITE_NOMEM;

            auto context = (fleeceFuncContext *)aux;
            new (&vtab->context) fleeceFuncContext(*context);
            *outVtab = vtab;
            return SQLITE_OK;
        }

        // Destructor for sqlite3_vtab
        static int disconnect(sqlite3_vtab *vtab) noexcept {
            free(vtab);
            return SQLITE_OK;
        }

        // Creates a new FleeceCursor object.
        static int open(sqlite3_vtab *vtab, sqlite3_vtab_cursor **outCursor) noexcept {
            *outCursor = new FleeceCursor((FleeceVTab *)vtab);
            return *outCursor ? SQLITE_OK : SQLITE_NOMEM;
        }

        // Frees a FleeceCursor.
        static int close(sqlite3_vtab_cursor *cursor) noexcept {
            delete (FleeceCursor *)cursor;
            return SQLITE_OK;
        }

        // "SQLite will invoke this method one or more times while planning a query
        // that uses this virtual table.  This routine needs to create
        // a query plan for each invocation and compute an estimated cost for that plan."
        static int bestIndex(sqlite3_vtab *vtab, sqlite3_index_info *info) noexcept {
            /* "Arguments on the virtual table name are matched to hidden columns in order. The number
           of arguments can be less than the number of hidden columns, in which case the latter
           hidden columns are unconstrained." */
            /* From json1.c: "The query strategy is to look for an equality constraint on the
           [`root_data`] column.  Without such a constraint, the table cannot operate." */
            /* In other words: If fl_each() is called with an argument, that manifests here as a
           constraint on the `root_data` column. A second argument results in a constraint on
           the `root_path` column. */
            int  rootDataIdx = -1, rootPathIdx = -1;
            auto constraint = info->aConstraint;
            for ( int i = 0; i < info->nConstraint; i++, constraint++ ) {
                if ( constraint->usable && constraint->op == SQLITE_INDEX_CONSTRAINT_EQ ) {
                    switch ( constraint->iColumn ) {
                        case kRootFleeceDataColumn:
                            rootDataIdx = i;
                            break;
                        case kRootPathColumn:
                            rootPathIdx = i;
                            break;
                        default: /* no-op */
                            break;
                    }
                }
            }
            // `info->idxNum` is used to communicate to the filter() function below; the value set here
            // will be passed to that function.
            // `argvIndex` specifies which constraint values will be passed as arguments to filter()
            // and in what order.
            if ( rootDataIdx < 0 ) {
                info->idxNum        = kNoIndex;
                info->estimatedCost = 1e99;
            } else {
                info->estimatedCost                           = 1.0;
                info->aConstraintUsage[rootDataIdx].argvIndex = 1;
                info->aConstraintUsage[rootDataIdx].omit      = 1;
                if ( rootPathIdx < 0 ) {
                    info->idxNum = kFleeceDataIndex;
                } else {
                    info->aConstraintUsage[rootPathIdx].argvIndex = 2;
                    info->aConstraintUsage[rootPathIdx].omit      = 1;
                    info->idxNum                                  = kPathIndex;
                }
            }
            return SQLITE_OK;
        }

#pragma mark - INSTANCE METHODS:

        FleeceCursor(FleeceVTab *vtab) : _vtab(vtab) {}

        void resetScope() noexcept {
            if ( _scope ) {
                auto data = _scope->data().buf;
                _scope.reset();
                if ( _scopeDataIsCopied ) {
                    ::free((void *)data);
                    _scopeDataIsCopied = false;
                }
            }
        }

        void reset() noexcept {
            resetScope();
            _rootPath      = nullslice;
            _container     = nullptr;
            _containerType = kNull;
            _rowCount      = 0;
            _rowid         = 0;
        }

        // This method is called to "rewind" the FleeceCursor object back
        // to the first row of output.  This method is always called at least
        // once prior to any call to column() or rowid() or eof().
        int filter(int idxNum, const char *idxStr, int argc, sqlite3_value **argv) noexcept {
            reset();
            if ( idxNum == kNoIndex ) return SQLITE_OK;

            // Parse the Fleece data:
            slice data;
            if ( idxNum == kPathIndex ) {
                // If fl_each is called with a 2nd (property path) argument, then the first arg is the
                // doc body, which we need to extract Fleece from:
                data = valueAsDocBody(argv[0], _scopeDataIsCopied);
            } else {
                data               = valueAsSlice(argv[0]);
                _scopeDataIsCopied = false;
            }
            if ( !data ) {
                // Weird not to get a document; have to return early to avoid a crash.
                // Treat this as an empty doc. (See issue #379)
                Warn("fleece_each filter called with null document! Query is likely to fail. (#379)");
                return SQLITE_OK;
            }
            _scope.emplace(data, _vtab->context.sharedKeys);

            _container = Value::fromTrustedData(data);
            if ( !_container ) {
                Warn("Invalid Fleece data in SQLite table");
                return SQLITE_MISMATCH;  // failed to parse Fleece data
            }

            // Evaluate the path, if there is one:
            if ( idxNum == kPathIndex ) {
                _rootPath = valueAsSlice(argv[1]);
                int rc    = evaluatePath(_rootPath, &_container);
                if ( rc != SQLITE_OK ) return rc;
            }

            // Determine the number of rows:
            if ( _container ) {
                _containerType = _container->type();
                switch ( _containerType ) {
                    case kArray:
                        _rowCount = _container->asArray()->count();
                        break;
                    case kDict:
                        _rowCount = _container->asDict()->count();
                        break;
                    default:
                        _rowCount = 1;
                        break;
                }
            }
            return SQLITE_OK;
        }

        // Return true if the cursor has been moved off of the last row of output;
        bool _atEOF() noexcept { return (_rowid >= _rowCount); }

        int atEOF() noexcept {
            if ( !_atEOF() ) return false;
            // Caller is going to wipe out the blob I'm parsing, so clear my Scope first
            resetScope();
            return true;
        }

        // Return values of columns for the row at which the FleeceCursor is currently pointing.
        int column(sqlite3_context *ctx, int column) noexcept {
            if ( _atEOF() ) return SQLITE_ERROR;
            switch ( column ) {
                case kKeyColumn:
                    setResultTextFromSlice(ctx, currentKey());
                    break;
                case kValueColumn:
                    setResultFromValue(ctx, currentValue());
                    break;
                case kTypeColumn:
                    {
                        auto value = currentValue();
                        sqlite3_result_int(ctx, (value ? value->type() : -1));
                        break;
                    }
                case kBodyColumn:
                    {
                        sqlite3_result_pointer(ctx, (void *)currentValue(), kFleeceValuePointerType, nullptr);
                        break;
                    }
                case kDataColumn:
                    setResultBlobFromEncodedValue(ctx, currentValue());
                    break;
#if 0  // these columns are used for the join but are never actually queried
            case kRootFleeceDataColumn:
                setResultBlobFromSlice(ctx, _fleeceData);
                break;
            case kRootPathColumn:
                setResultTextFromSlice(ctx, _rootPath);
                break;
#endif
                default:
                    Warn("fl_each: Unexpected column(%d)", column);
                    return SQLITE_ERROR;
            }
            return SQLITE_OK;
        }

        const slice currentKey() noexcept {
            const Dict *dict = _container->asDict();
            if ( !dict ) return nullslice;
            Dict::iterator iter(dict);
            iter += _rowid;
            return iter.keyString();
        }

        const Value *currentValue() noexcept {
            switch ( _containerType ) {
                case kArray:
                    return _container->asArray()->get(_rowid);
                case kDict:
                    {
                        Dict::iterator iter(_container->asDict());
                        iter += _rowid;
                        return iter.value();
                    }
                default:
                    return _container;  // only one row, that's the root value
            }
        }

        // Return the rowid for the current row.
        int rowid(int64_t *outRowid) noexcept {
            *outRowid = _rowid;
            return SQLITE_OK;
        }

        // Advance a FleeceCursor to its next row of output.
        int next() noexcept {
            ++_rowid;
            (void)atEOF();  // Clear _scope on EOF, before caller frees the Fleece blob
            return SQLITE_OK;
        }

#pragma mark - SQLITE3 HOOK FUNCTIONS:

        static int cursorNext(sqlite3_vtab_cursor *cur) noexcept { return ((FleeceCursor *)cur)->next(); }

        static int cursorColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i) noexcept {
            return ((FleeceCursor *)cur)->column(ctx, i);
        }

        static int cursorRowid(sqlite3_vtab_cursor *cur, long long *outRowid) noexcept {
            return ((FleeceCursor *)cur)->rowid((int64_t *)outRowid);
        }

        static int cursorEof(sqlite3_vtab_cursor *cur) noexcept { return ((FleeceCursor *)cur)->atEOF(); }

        static int cursorFilter(sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr, int argc,
                                sqlite3_value **argv) noexcept {
            return ((FleeceCursor *)cur)->filter(idxNum, idxStr, argc, argv);
        }


      public:
        // Module definition of 'fl_each' function
        constexpr static sqlite3_module kEachModule = {
                0,            /* iVersion */
                0,            /* xCreate */
                connect,      /* xConnect */
                bestIndex,    /* xBestIndex */
                disconnect,   /* xDisconnect */
                0,            /* xDestroy */
                open,         /* xOpen - open a cursor */
                close,        /* xClose - close a cursor */
                cursorFilter, /* xFilter - configure scan constraints */
                cursorNext,   /* xNext - advance a cursor */
                cursorEof,    /* xEof - check for end of scan */
                cursorColumn, /* xColumn - read data */
                cursorRowid,  /* xRowid - read data */
                0,            /* xUpdate */
                0,            /* xBegin */
                0,            /* xSync */
                0,            /* xCommit */
                0,            /* xRollback */
                0,            /* xFindMethod */
                0,            /* xRename */
        };

    };  // end class definition

    constexpr sqlite3_module FleeceCursor::kEachModule;

    int RegisterFleeceEachFunctions(sqlite3 *db, const fleeceFuncContext &context) {
        return sqlite3_create_module_v2(db, "fl_each", &FleeceCursor::kEachModule, new fleeceFuncContext(context),
                                        [](void *param) { delete (fleeceFuncContext *)param; });
    }


}  // namespace litecore
