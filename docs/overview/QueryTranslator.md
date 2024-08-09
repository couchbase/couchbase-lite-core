#  The Query Translator

The Query Translator's job is to convert a query from our [JSON schema][SCHEMA] into SQLite-flavor SQL.

It operates in three passes:
1. Traverse the JSON/Fleece tree creating a tree of `Node` objects (q.v.)
2. Do a bit of postprocessing on the `Node` tree
3. Traverse the `Node`s, writing SQL to an output stream.

## Nodes

- _`Node`_
  - _`AliasedNode`_ -- an item that can be named with "`AS` ..."
    - `SourceNode` -- an item in the `FROM` clause, i.e. a collection, UNNEST, or table-based index
    - `WhatNode` -- an item in the `WHAT` clause, i.e. a result
  - _`ExprNode`_ -- an expression
    - `CollateNode` -- a `COLLATE` expression
    - `FunctionNode` -- a function call
    - _`IndexedNode`_ -- an expression related to an index
      - _`FTSNode`_
        - `MatchNode` -- an FTS `match()` call
        - `RankNode` -- an FTS `rank()` call
      - `VectorDistanceNode` -- a vector-search `approx_vector_distance()` call
    - `LiteralNode` -- a literal value
    - `MetaNode` -- a `meta()` function or property thereof (`id`, `sequence`, etc.)
    - `OpNode` -- Most of the other operations in an expression, like `AND`, `+`, etc.
      - `AnyEveryNode` -- an `ANY`, `EVERY` or `ANY AND EVERY` expression
    - `ParameterNode` -- a query parameter, those things prefixed with `$`
    - `PropertyNode` -- a reference to a document property
    - `RawSQLNode` -- just a raw string to insert into the SQL (added during postprocessing)
    - `SelectNode` -- a `SELECT` statement
      - `QueryNode` -- the top-level query
    - `VariableNode` -- a temporary variable in an `ANY`, usually prefixed with `?`

The specific operation in an `OpNode` is identified by an `Operation` struct from `kOperationList`.

The specific function of a `FunctionNode` is identified by an `FunctionSpec` struct from `kFunctionList`.

# Memory Management

Nodes are single-owner: every `Node` is owned by a `unique_ptr` held by its parent.

Some Nodes need to reference Nodes other than their children: for instance, a `PropertyNode` points to the `SourceNode` representing the collection. Also, every Node points to its parent since during the postprocessing phase it's sometimes necessary for a Node to look at its context.

These references were originally raw pointers, but I was worried about the potential for use-after-free bugs. I thought about changing from `unique_ptr` to `shared_ptr` or `Retained`, but that would create cycles.

What I've ended up doing is creating a new utility class `checked_ptr` (see LiteCore/Support/checked_ptr.hh). This has mostly the same semantics as a raw pointer, except that it has to point to an object inheriting from `checked_target`. A `checked_target` tracks the number of `checked_ptr`s pointing to it, and its destructor asserts that the number is zero.

While this doesn't prevent dangling references, it detects them instantly and makes them into fatal errors. This has worked pretty well so far, but if it turns out to be problematic we can always switch to `Retained` and clean up cycles during tear-down of the tree.

[SCHEMA]: https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
