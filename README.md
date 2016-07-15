**CBForest** is a higher-level C and C++ wrapper around [ForestDB][FDB], a new key-value storage engine based on a hierarchical B+-tree trie data structure. ForestDB is similar in functionality to the [CouchStore][COUCHSTORE] engine that currently underlies Couchbase Server, but it should be faster and more space-efficient, especially on solid-state disks (SSDs).

CBForest adds an idiomatic object-oriented C++ API for ForestDB, and also some new functionality (see below.) Recently a C API with the same functionality has been added, and Java and C# bindings to that C API.

The immediate purpose of CBForest is to serve as the storage engine of the next generation of [Couchbase Lite][CBL] (on all platforms), replacing SQLite. But it may find other uses too, perhaps for applications that want a fast minimalist data store with map/reduce indexing, but don't need any of the fancy features of Couchbase Lite like replication.

## Features

* ForestDB features, available via idiomatic C++, Java and C# APIs:
    * Fast key-value storage, where keys and values are both opaque blobs.
    * Extremely robust append-only file format with write-ahead log and automatic compaction.
    * Reads are never blocked, even while writes or transactions are in progress.
    * Iteration by key order.
    * Iteration by _sequence_, reflecting the order in which changes were made to the database. (This is useful for tasks like updating indexes and replication.)
    * Database encryption using AES-256.

* New features implemented by CBForest:
    * Optional multi-version document format that keeps a revision tree of the history of each document (as in Couchbase Lite or CouchDB.)
    * Index API that uses a database as an index of an external data set.
    * Map-reduce indexes that update incrementally as documents are changed in the source DB (as in Couchbase Lite or CouchDB.)
    * Limited full-text indexing.
    * Limited geo-indexing.
    * Support for JSON-compatible structured keys in indexes, sorted according to CouchDB's JSON collation spec.

## Platform Support

CBForest runs on Mac OS, iOS, tvOS, Android, various other flavors of Unix, and Windows.

CBForest has been in use since mid-2015 in the iOS/Mac version of [Couchbase Lite][CBL] 1.1, and since early 2016 in the 1.2 release on all the above platforms.

## License

Like all Couchbase source code, this is released under the Apache 2 license.

[FDB]: https://github.com/couchbase/forestdb
[CBL]: http://www.couchbase.com/nosql-databases/couchbase-mobile
[COUCHSTORE]: https://github.com/couchbase/couchstore
