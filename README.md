**Couchbase Lite Core** (aka **LiteCore**) is the next-generation core storage and query engine for [Couchbase Lite][CBL]. It provides a cross-platform implementation of the database CRUD and query features, document versioning, and replication/sync.

All platform implementations of Couchbase Lite (from 2.0 onward) are built atop this core, adding higher-level language & platform bindings. But LiteCore may find other uses too, perhaps for applications that want a fast minimalist data store with map/reduce indexing and queries, but don't need the higher-level features of Couchbase Lite.

# Features

* Database CRUD (Create, Read, Update, Delete) operations:
    * Fast key-value storage, where keys and values are both opaque blobs
    * Iteration by key order
    * Iteration by _sequence_, reflecting the order in which changes were made to the database. (This is useful for tasks like updating indexes and replication)
    * Multi-version document format that tracks history using a revision tree (as in CouchDB)
    * Timed document expiration (as in Couchbase Server)
    * API support for database encryption (as provided by SQLCipher or SQLite's Encryption Extension)
    * Highly efficient [Fleece][FLEECE] binary data encoding: supports JSON data types but
      requires no parsing, making it extremely efficient to read
* Direct querying of schemaless JSON documents:
    * Semantics based on SQL; supports most [N1QL][N1QL] functionality
    * JSON query syntax, similar to a parse tree; easy to generate from platform APIs like NSPredicate or LINQ
    * Can search and index arbitrary document properties without requiring any schema
    * Queries compile into SQL and from there into SQLite compiled bytecode
    * Parameters can be substituted without having to recompile the query
    * Queries don't require indexes, but will run faster if indexes are created on the document
      properties being searched for
    * Supports full-text search, using SQLite's FTS4 module
* Replicator:
    * Multi-master bidirectional document sync
    * Replicator detects conflicts; temporarily stores both revisions, and notifies app-level handlers to resolve them
    * Uses [BLIP][BLIP] multiplexing protocol over WebSockets
    * Pluggable transports mean it could run over Bluetooth or other protocols
* REST API:
    * Implements a _subset_ of the CouchDB / Sync Gateway / Couchbase Lite REST API
    * Currently incomplete; not ready for prime time
* Pluggable storage engines:
    * SQLite is available by default
    * Others could be added by implementing C++ `DataFile`, `KeyStore`, `Query` interfaces
* C and C++ APIs
* Bindings to C# and Java

# Platform Support

LiteCore runs on Mac OS, iOS, tvOS, Android, various other flavors of Unix, and Windows.

It is written in C++ (using C++11 features) and compiles with Clang and MSVC.

# Status

**As of April 2018:** LiteCore has gone GA as a component of Couchbase Lite 2.0! Development continues...

* Active development usually happens on the `master` branch, which may therefore be temporarily broken. We don't currently have a "stable" branch.
* Most development is done on macOS using Xcode, so the Xcode project should always build, and the code should pass its unit tests on Mac. iOS is pretty likely to work too, since it's so similar to Mac at this level.
* The CMake build is generally up to date but may fall behind.  CMake can be used to build every variant except for iOS and tvOS.

# Building It

**Very Important:**

* This repo has **submodules**. Make sure they're checked out. Either use `git clone --recursive` to download LiteCore, or else after the clone run `git submodule update --init --recursive`.

Once you've cloned or downloaded the source tree...

## macOS, iOS

If you want to use Objective-C or Swift APIs, you should use Couchbase Lite instead — check out and build the `feature/2.0` branch of the [couchbase-lite-ios][CBL_iOS_2] repo, which itself includes LiteCore as a submodule. The following instructions are to build just LiteCore on its own:

* Make sure you have Xcode **9.2** or later. 
* Open **Xcode/LiteCore.xcodeproj**. 
* Select the scheme **LiteCore dylib**. 
* Build.

## Linux, Android

**Note** Android requires CMake 3.7 or higher!

**Important!** LiteCore uses a couple of external libraries, which may or may not be installed in your system already. If not, please install the appropriate development packages via your package manager. You must have the following libraries present:
    
- libz
- libicu

You'll need **Clang 3.9.1 or higher**. Unfortunately a lot of distros only have 3.5; run `clang --version` to check, and upgrade manually if necessary. You also need a corresponding version of libc++. On Debian-like systems, the apt-get packages you need are `clang`, `libc++1`, `libc++-dev`, `libc++abi-dev`.

### Actually Building

Once you've got the dependencies and compiler installed, do this from the root directory of the source tree:

    cd build_cmake/scripts
    ./build_unix.sh

If CMake's initial configuration checks fail, the setup may be left in a broken state and will then fail immediately. To remedy this:

    rm -r ../unix
    ./build_unix.sh

## Windows Desktop

Open the Visual Studio 2015 Developer Command Prompt and navigate to the repo root.  Then execute:
    
    * 64-bit build *
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" -G "Visual Studio 14 2015 Win64" ..
    
    * 32-bit build *
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" -G "Visual Studio 14 2015" ..
    
This will create `LiteCore.sln` in the directory that you can open with Visual Studio.

## Windows Store

Open the Visual Studio 2015 Developer Command Prompt and navigate to the repo root.  Then execute:
    
    * x64 build *
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" -G "Visual Studio 14 2015 Win64" -DCMAKE_SYSTEM_NAME=WindowsStore
    -D CMAKE_SYSTEM_VERSION="10.0.14393.0" ..
    
    * x86 build *
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" -G "Visual Studio 14 2015" -DCMAKE_SYSTEM_NAME=WindowsStore
    -D CMAKE_SYSTEM_VERSION="10.0.14393.0" ..
    
    * ARM build *
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" -G "Visual Studio 14 2015 ARM" -DCMAKE_SYSTEM_NAME=WindowsStore
    -D CMAKE_SYSTEM_VERSION="10.0.14393.0" ..
    
This will create `LiteCore.sln` in the directory that you can open with Visual Studio.

# Documentation

## API

We have [online C API documentation](https://couchbase.github.io/couchbase-lite-core/C/html/modules.html), generated by Doxygen from the headers.

If this is out of date, or you want a local copy, you can generate your own by running the following commands from a shell at the root directory of the repo:

    cd C
    doxygen
    
The main page is then located at `../docs/C/html/modules.html`.

**The C API is considered unstable** and may change without notice, since it's considered an internal API of Couchbase Lite. In the future we want to provide a stable and supported C/C++ API, but not yet.

**Do not call any C++ APIS** -- these are the underlying implementation beneath the C API. They are even more unstable, expose internal functionality we don't support, and may blow up if used incorrectly. The exception is `c4.hh`, which provides some handy C++ wrappers around the C API and will make your life more pleasant if you code in C++.

## Internal Implementation

For those interested in diving into the implementation, there is [an overview of the major classes](https://github.com/couchbase/couchbase-lite-core/blob/master/docs/overview/index.md).

# Authors

Jens Alfke ([@snej](https://github.com/snej)), Jim Borden ([@borrrden](https://github.com/borrrden)), Hideki Itakura ([@hideki](https://github.com/hideki))

# License

Like all Couchbase open source code, this is released under the Apache 2 [license](LICENSE).

[CBL]: http://www.couchbase.com/nosql-databases/couchbase-mobile
[N1QL]: https://www.couchbase.com/n1ql
[FLEECE]: https://github.com/couchbaselabs/fleece
[BLIP]: https://github.com/couchbaselabs/BLIP-Cpp/blob/master/docs/BLIP%20Protocol.md
