**CBForest** is an Objective-C wrapper around [ForestDB][FDB], a new key-value storage engine based on a hierarchical B+-tree trie data structure. ForestDB is similar in functionality to the [CouchStore][COUCHSTORE] engine that currently underlies Couchbase Server, but it should be faster and more space-efficient, especially on solid-state disks (SSDs).

The immediate purpose of CBForest is to serve as the storage engine of the next generation of [Couchbase Lite][CBL] for iOS, replacing SQLite. But it may find other uses too, perhaps for Mac or iOS apps that want a fast minimalist data store but don't need any of the fancy features of Couchbase Lite like replication.

## Features

* Fast key-value storage, where keys and values are both opaque blobs.
* Iteration by key order.
* Iteration by _sequence_, reflecting the order in which changes were made to the database. (This can be useful for updating indexes or for replication.)
* Optional multi-version storage that keeps a revision tree of the history of each document (as in Couchbase Lite or CouchDB.)
* Index API that uses a database as an index of an external data set.
* Map-reduce indexes that update incrementally as documents are changed in the source DB (as in Couchbase Lite or CouchDB.)

## Status

As of this writing (8 April 2014), this is very much a work in progress, like ForestDB itself. Definitely pre-alpha! There will be lots of changes. Who knows, the project might even be scrapped, or merged directly into Couchbase Lite. The future is unwritten.

So far there is an Objective-C API that provides most of the functionality that Couchbase Lite will need. There are unit tests that exercise a lot of the API, but the code hasn't been pushed very hard yet.

## License

Like all Couchbase source code, this is released under the Apache 2 license.

[FDB]: https://github.com/couchbaselabs/forestdb
[CBL]: https://github.com/couchbase/couchbase-lite-ios
[COUCHSTORE]: https://github.com/couchbaselabs/couchstore