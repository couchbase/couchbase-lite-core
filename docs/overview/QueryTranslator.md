#  The Query Translator

The Query Translator's job is to convert a query from our [JSON schema][SCHEMA] into SQLite-flavor SQL.

It operates in three passes:
1. Traverse the JSON/Fleece tree creating a tree of Node objects (q.v.)
2. Do a bit of postprocessing on the Node tree
3. Traverse the Nodes, writing SQL to an output stream.

## Nodes

- _`Node`_
  - _`AliasedNode`_
    - `SourceNode` -- an item in the `FROM` clause
    - `WhatNode` -- an item in the `WHAT` clause, i.e. a result
  - _`ExprNode`_
    - `CollateNode` -- a `COLLATE` expression
    - `FunctionNode` -- a function call
    - _`IndexedNode`_
      - `MatchNode` -- an FTS `match()` call
      - `RankNode` -- an FTS `rank()` call
      - `VectorMatchNode` -- a vector-search `vector_match()` call
      - `VectorDistanceNode` -- a vector-search `vector_distance()` call
    - `LiteralNode` -- a literal value
    - `MetaNode` -- a `meta()` function or property thereof (`id`, `sequence`, etc.)
    - `OpNode` -- Most of the other operations in an expression, like `AND`, `+`, etc.
      - `AnyEveryNode` -- an `ANY`, `EVERY` or `ANY AND EVERY` expression
    - `ParameterNode` -- a query parameter, those things prefixed with `$`
    - `PropertyNode` -- a reference to a document property
    - `RawSQLNode` -- just a raw string to insert into the SQL (added during postprocessing)
    - `SelectNode` -- a `SELECT`
      - `QueryNode` -- the top-level query
    - `VariableNode` -- a temporary variable in an `ANY`, usually prefixed with `?`

The specific operation in an `OpNode` is identified by an `Operation` struct from `kOperationList`.

The specific function of a `FunctionNode` is identified by an `FunctionSpec` struct from `kFunctionList`.

[SCHEMA]: https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
