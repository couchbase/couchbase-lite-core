#  The Query Translator

The Query Translator's job is to convert a query from our [JSON schema][SCHEMA] into SQLite-flavor SQL. It replaces the old QueryParser.

It operates in several passes:

1. Traverse the JSON/Fleece tree creating a tree of `Node` objects (q.v.)
2. Do a bit of postprocessing on the tree.
3. For each `SourceNode`, ask the delegate what its SQL table name is and store that in its `tableName` property.
4. Traverse the `Node`s, writing SQL to an output stream.

> If you want to know what happens when the query is run, and why the translator emits calls to cryptic SQL functions, do read the [Query Runtime](QueryRuntime.md) documentation.


## QueryTranslator

This class is the interface to the rest of LiteCore, and is the only API in this directory that should be called from the outside. Its API is very close to that of the old `QueryParser`, to avoid having to change much outside code.


## Nodes

`Node` objects form an AST (Abstract Syntax Tree) generated from the JSON/Fleece input. They do most of the work. The parsing is driven by `ExprNode`'s various `parse...` methods. SQL generation happens in the `writeSQL` methods.

Here's the class hierarchy, with abstract classes italicized:

- _`Node`_
  - _`AliasedNode`_ -- an item that can be named with "`AS` ..."
    - `SourceNode` -- an item in the `FROM` clause; SourceNode itself is used for regular collections
      - `IndexSourceNode` -- a table-based index implicitly added to the tree by a FTS or vector-search function
      - `UnnestSourceNode` -- an UNNEST expression
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
    - `ParameterNode` -- a query parameter: those variables prefixed with `$`
    - `PropertyNode` -- a reference to a document property
    - `RawSQLNode` -- just a raw string to insert into the SQL (added during postprocessing)
    - `SelectNode` -- a `SELECT` statement
    - `VariableNode` -- a temporary variable in an `ANY`, usually prefixed with `?`

The specific operation of an `OpNode` is identified by an `Operation` struct from `kOperationList`.

The specific function of a `FunctionNode` is identified by an `FunctionSpec` struct from `kFunctionList`.


## Memory Management

The Node tree ends up being full of cycles: Nodes point to children, children point back up at parents, and there are sideways links too, like that from a PropertyNode to the SourceNode of the collection it references.

I originally used `unique_ptr` for the links to children, and plain pointers for the rest, but was worried that it could lead to use-after-free bugs.

I ended up switching to an [**arena allocator**][ARENA]. All `Node` objects are allocated by it; then when the QueryTranslator is done it frees the arena. This makes the code simpler, with no `unique_ptr<>` or `Retained<>` nonsense: you allocate a node with e.g. `new (ctx) ExprNode(...)`; node references are typed `Node*`; and there's no need to delete nodes. Very old school!

The downside is that, since the arena never calls destructors, **`Node` and its subclasses cannot declare any data members that require destruction.** Otherwise they'd leak memory! That rules out using `std::string`, `std::vector`, `fleece::MutableArray`, etc. There are a few utilities to mitigate this:

- `ParseContext::newString()` allocates a C string in the arena.
- `List<T>` is a simple intrusive linked-list for Nodes. Several of the subclasses use this to create dynamic lists of their child nodes.


[SCHEMA]: https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
[ARENA]: https://en.wikipedia.org/wiki/Region-based_memory_management
