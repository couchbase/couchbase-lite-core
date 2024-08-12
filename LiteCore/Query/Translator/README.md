#  The Query Translator

The Query Translator's job is to convert a query from our [JSON schema][SCHEMA] into SQLite-flavor SQL. It replaces the old QueryParser.

It operates in several passes:
1. Traverse the JSON/Fleece tree creating a tree of `Node` objects (q.v.)
2. Do a bit of postprocessing on the `Node` tree.
3. For each `SourceNode`, ask the delegate what its SQL table name is and store that in the node.
4. Traverse the `Node`s, writing SQL to an output stream.

## QueryTranslator

This class is the interface to the rest of LiteCore, and the only API in this directory that should be called from the outside. Its API is very close to that of the old `QueryParser`, to avoid having to change much outside code.

## Nodes

Node objects form an AST (Abstract Syntax Tree) generated from the Fleece. They do most of the work. In particular, the parsing is driven by `ExprNode`'s various `parse...` methods.

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
    - `ParameterNode` -- a query parameter, those things prefixed with `$`
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

The only tricky part is the cleanup. Unfortunately some Node subclasses have `string` or `vector` members that allocate memory, so to avoid leaks it's necessary to call each Node's destructor before freeing the arena. The `RootContext` destructor takes care of this by iterating every block in the arena, casting it to `Node*` and destructing it.

Unfortunately there's a special case that happens when a Node constructor throws an exception as a result of a parse error. The C++ runtime code destructs the partly-completed object and tells the allocator to delete the block, but the arena doesn't have a notion of a free block, so `free` is usually a no-op. To prevent the cleanup code (previons paragraph) from double-destructing, the pseudo-"free" code zeroes the first bytes of the block to identify it as not being a real object anymore.

(The ideal solution would be to allocate everything including `string`s and `vector`s in the arena; then there'd be no need to run any destructors. That would require figuring out the C++ allocator APIs.)

[SCHEMA]: https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
[ARENA]: https://en.wikipedia.org/wiki/Region-based_memory_management
