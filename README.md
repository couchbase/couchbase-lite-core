**Couchbase Lite Core** (aka **LiteCore**) is the next-generation core storage and query engine for [Couchbase Lite][CBL]. It provides a cross-platform implementation of the database CRUD and query features, as well as document versioning.

All platform implementations of Couchbase Lite 2.0 will be built atop this core, adding replication and higher-level language & platform bindings. But LiteCore may find other uses too, perhaps for applications that want a fast minimalist data store with map/reduce indexing, but don't need the higher-level features of Couchbase Lite.

**THIS IS NOT A RELEASED PRODUCT. THIS IS NOT FINISHED CODE, OR EVEN ALPHA.** This is currently (August 2016) a work in progress. See "Status" section below.

## Features

* Database CRUD (Create, Read, Update, Delete) operations:
    * Fast key-value storage, where keys and values are both opaque blobs
    * Iteration by key order
    * Iteration by _sequence_, reflecting the order in which changes were made to the database. (This is useful for tasks like updating indexes and replication.)
    * Optional multi-version document format that tracks history using a revision tree (as in CouchDB) or version vectors
    * Timed document expiration (as in Couchbase Server)
    * API support for database encryption (as provided by ForestDB and SQLCipher)
* Indexing & querying:
    * Index API that uses a database as an index of an external data set
    * Map-reduce indexes that update incrementally as documents are changed in the source DB (as in Couchbase Lite or CouchDB)
    * Limited full-text indexing
    * Limited geo-indexing
    * JSON-compatible structured keys in indexes, sorted according to CouchDB's JSON collation spec
    * Querying by key range, with typical options like descending order, offset, limit
* Pluggable storage engines
    * [ForestDB][FDB] and SQLite are available by default
    * Others can be added by implementing C++ `DataFile` and `KeyStore` interfaces
* C++, C, Java and C# APIs

## Platform Support

LiteCore runs on Mac OS, iOS, tvOS, Android, various other flavors of Unix, and Windows.

It is written in C++ (using C++11 features) and compiles with Clang, GCC and MSVC.

An earlier version of LiteCore, known as CBForest, has been in use since mid-2015 in the iOS/Mac version of [Couchbase Lite][CBL] 1.1, and since early 2016 in the 1.2 release on all the above platforms.

## Status

**As of August 2016:** Under heavy development. Watch out for falling I-beams! As the primary development platform is Mac OS, only the Xcode project is likely to build, for the time being. Visual Studio and command-line GCC builds will catch up in the future.

## Authors

Jens Alfke ([@snej](https://github.com/snej)), Jim Borden ([@borrrden](https://github.com/borrrden)), Hideki Itakura ([@hideki](https://github.com/hideki))

## License

Like all Couchbase source code, this is released under the Apache 2 [license](LICENSE).

[FDB]: https://github.com/couchbase/forestdb
[CBL]: http://www.couchbase.com/nosql-databases/couchbase-mobile
