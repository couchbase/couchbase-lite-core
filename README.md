**Couchbase Lite Core** (aka **LiteCore**) is the next-generation core storage and query engine for [Couchbase Lite][CBL]. It provides a cross-platform implementation of the database CRUD and query features, document versioning, and replication/sync.

All platform implementations of Couchbase Lite 2.0 are built atop this core, adding higher-level language & platform bindings. But LiteCore may find other uses too, perhaps for applications that want a fast minimalist data store with map/reduce indexing and queries, but don't need the higher-level features of Couchbase Lite.

**THIS IS NOT A RELEASED PRODUCT. THIS IS NOT FINISHED CODE.** As of March 2017, the status is roughly "alpha". We expect a stable 1.0 release later in 2017. See "Status" section below.

# Features

* Database CRUD (Create, Read, Update, Delete) operations:
    * Fast key-value storage, where keys and values are both opaque blobs
    * Iteration by key order
    * Iteration by _sequence_, reflecting the order in which changes were made to the database. (This is useful for tasks like updating indexes and replication.)
    * Optional multi-version document format that tracks history using a revision tree (as in CouchDB) or version vectors
    * Timed document expiration (as in Couchbase Server)
    * API support for database encryption (as provided by SQLCipher or SQLite's Encryption Extension)
    * Highly efficient [Fleece][FLEECE] binary data encoding: supports JSON data types but
      requires no parsing, making it extremely efficient to read.
* Map/reduce indexing & querying:
    * Index API that uses a database as an index of an external data set
    * Map-reduce indexes that update incrementally as documents are changed in the source DB (as in Couchbase Lite or CouchDB)
    * JSON-compatible structured keys in indexes, sorted according to CouchDB's JSON collation spec
    * Querying by key range, with typical options like descending order, offset, limit
* Direct database querying based on N1QL:
    * Currently only supported in SQLite-based databases
    * Supports most [N1QL][N1QL] functionality
    * JSON-based query syntax, similar to a parse tree; easy to generate from platform APIs like NSPredicate
    * Can search and index arbitrary document properties without requiring any schema
    * Queries compile into SQL and from there into SQLite compiled bytecode
    * Parameters can be substituted without having to recompile the query
    * Queries don't require indexes, but will run faster if indexes are created on the document
      properties being searched for.
    * Supports full-text search, using SQLite's FTS4 module.
* Pluggable storage engines
    * SQLite is available by default
    * Others can be added by implementing C++ `DataFile` and `KeyStore` interfaces
* C and C++ APIs
* Bindings to C# and Java (may not be up-to-date with the native APIs)

# Platform Support

LiteCore runs on Mac OS, iOS, tvOS, Android, various other flavors of Unix, and Windows.

It is written in C++ (using C++11 features) and compiles with Clang (with libc++), GCC 5+ and MSVC.

# Status

**As of March 2017:** Under heavy development. Almost all features are implemented, and seem pretty solid, but APIs may still change and there may be short-term build problems if one compiler or another dislikes a recent commit. 

* The primary development platform is macOS, so the Xcode project should always build, and the code should pass its unit tests on Mac. iOS is pretty likely to work too ,since it's so similar to Mac at this level.
* The CMake build is generally up to date but may fall behind.
* The C# bindings are updated within a few days of C API changes.
* The Java bindings are in progress but may not be fully functional yet.

# Building It

**Very Important:**

* This repo has **submodules**. Make sure they're checked out. Either use `git clone --recursive` to download LiteCore, or else after the clone run `git submodule update --init --recursive`.

Once you've cloned or downloaded the source tree...

## macOS, iOS

If you want to use the Objective-C or Swift APIs, you should instead check out and build the `feature/2.0` branch of the [couchbase-lite-ios][CBL_iOS_2] repo, which itself includes LiteCore as a submodule. The following instructions are to build just LiteCore on its own:

* Make sure you have Xcode **8.3** or later. 
* Open **Xcode/LiteCore.xcodeproj**. 
* Select the scheme **LiteCore dylib**. 
* Build.

## Linux, Android

### Dependencies

**Note** Android requires CMake 3.7 or higher!

**Important!** LiteCore uses a couple of external libraries, which may or may not be installed in your system already. If not, please install the appropriate development packages via your package manager. This is especially necessary on Ubuntu, which comes without the development packages for common libraries like SQLite and OpenSSL. You must have the following libraries present:
    
| LIB_NAME   | APT-GET        | YUM           | SOURCE |
| ---------- | -------------- | ------------- | ------ |
| libsqlite3 | libsqlite3-dev | n/a           | https://sqlite.org/download.html          |
| libcrypto  | libssl-dev     | openssl-devel | https://github.com/openssl/openssl        |
| libbsd     | libbsd-dev     | libbsd        | https://libbsd.freedesktop.org/releases/  |

### CentOS Users

The compiler currently in the CentOS repos (GCC 4.8.5) is too old to build this library.  You will need to get a later version by doing the following:

```
#One time
sudo yum install centos-release-scl
sudo yum install devtoolset-3-gcc* #devtoolset-3 == 4.9.2, devtoolset-4 == 5.2.1 (don't forget the trailing '*')

#After each login
scl enable devtoolset-3 bash #or devtoolset-4 
```

### Actually Building

Once you've got the dependencies and compiler installed, do this from the root directory of the source tree:

    cd build_cmake
    ./build.sh

## Windows

Open the Visual Studio 2015 Developer Command Prompt and navigate to the repo root.  Then execute:
    
    * 64-bit build *
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" -G "Visual Studio 14 2015 Win64" ..
    
    * 32-bit build *
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" ..
    
This will create `LiteCore.sln` in the directory that you can open with Visual Studio.

# Documentation

We have [online C API documentation](https://couchbase.github.io/couchbase-lite-core/C/html/modules.html), generated by Doxygen from the headers.

If this is out of date, or you want a local copy, you can generate your own by running the following commands from a shell at the root directory of the repo:

    cd C
    doxygen
    
The main page is then located at `Documentation/html/modules.html`.

# Authors

Jens Alfke ([@snej](https://github.com/snej)), Jim Borden ([@borrrden](https://github.com/borrrden)), Hideki Itakura ([@hideki](https://github.com/hideki))

# License

Like all Couchbase open source code, this is released under the Apache 2 [license](LICENSE).

[CBL]: http://www.couchbase.com/nosql-databases/couchbase-mobile
[CBL_iOS_2]: https://github.com/couchbase/couchbase-lite-ios/tree/feature/2.0
[N1QL]: https://www.couchbase.com/n1ql
[FLEECE]: https://github.com/couchbaselabs/fleece
