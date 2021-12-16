**Couchbase Lite Core** (aka **LiteCore**) is the next-generation core storage and query engine for [Couchbase Lite][CBL]. It provides a cross-platform implementation of the database CRUD and query features, document versioning, and replication/sync.

All platform implementations of Couchbase Lite (from 2.0 onward) are built atop this core, adding higher-level language & platform bindings.

**IMPORTANT:** We do _not_ recommend (or support) using LiteCore directly in other projects. Its API is unstable and can be tricky to use. Instead, use [Couchbase Lite for C][CBL_C], a cross-platform version of Couchbase Lite with a C (and C++) API.

![Travis CI status](https://travis-ci.org/couchbase/couchbase-lite-core.svg?branch=master)

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
    * N1QL parser that translates to the above JSON syntax
    * Can search and index arbitrary document properties without requiring any schema
    * Can index arrays in documents, enabling efficient denormalized one-to-many relations
    * Queries compile into SQL and from there into SQLite compiled bytecode
    * Parameters can be substituted without having to recompile the query
    * Queries don't require indexes, but will run faster if indexes are created on the document
      properties being searched
    * Supports full-text search, using SQLite's FTS4 module
* Replicator:
    * Multi-master bidirectional document sync
    * Replicator detects conflicts; temporarily stores both revisions, and notifies app-level handlers to resolve them
    * Replicator transfers document deltas, saving bandwidth when updating large documents
    * Uses [BLIP][BLIP] multiplexing protocol over WebSockets
    * Pluggable transports mean it could run over Bluetooth or other protocols
* REST API:
    * Implements a _subset_ of the CouchDB / Sync Gateway / Couchbase Lite REST API
    * Currently incomplete; not ready for prime time
* Pluggable storage engines:
    * SQLite is available by default
    * Others could be added by implementing C++ `DataFile`, `KeyStore`, `Query` interfaces
* Command-line `cblite` tool
    * Easy database inspection, document lookups and queries
    * Can run replications (push and/or pull)
    * Can serve a CouchDB-like REST API over HTTP
* C and C++ APIs (rather low-level; not considered "public" APIs yet.)
* Bindings to C# and Java

# Platform Support

LiteCore runs on Mac OS, iOS, Android, various other flavors of Unix, and Windows.

It is written in C++ (using C++17 features) and compiles with Clang, G++ and MSVC.

It has been experimentally built and run on the Raspberry Pi but this is not an actively maintained use.

# Status

**As of June 2020:** LiteCore is in active use as the engine of Couchbase Lite 2! Development continues...

* Active development usually happens on the `master` branch, which may therefore be temporarily broken. We don't currently have a "stable" branch.
* Most development is done on macOS using Xcode, so the Xcode project should always build, and the code should pass its unit tests on Mac. iOS is pretty likely to work too, since it's so similar to Mac at this level.
* The CMake build is generally up to date but may fall behind.  CMake can be used to build every variant except for iOS.

# Building It

We do _not_ recommend (or support) using LiteCore directly in other projects. Its API is unstable and can be tricky to use. (Instead see [Couchbase Lite for C][CBL_C], a cross-platform version of Couchbase Lite with a C (and C++) API.) The build instructions here are for the benefit of developers who want to debug or extend Couchbase Lite at the LiteCore level.

**Very Important:**

* This repo has **submodules**. Make sure they're checked out. Either use `git clone --recursive` to download LiteCore, or else after the clone run `git submodule update --init --recursive`.

Once you've cloned or downloaded the source tree...

## macOS, iOS

If you want to use Objective-C or Swift APIs, you should use Couchbase Lite instead — check out and build the [couchbase-lite-ios][CBL_iOS_2] repo, which itself includes LiteCore as a submodule.

The following instructions are to build just LiteCore on its own:

* Make sure you have Xcode **12.2** or later (:warning: Do not use Xcode 13 because of a [downstream issue](https://github.com/ARMmbed/mbedtls/issues/5052) :warning:).
* Open **Xcode/LiteCore.xcodeproj**. 
* Select the scheme **LiteCore static** or **LiteCore dylib**. 
* Choose _Product>Build_ (for a debug build) or _Product>Build For>Profiling_ (for a release/optimized build).
* Link the build product `libLiteCoreStatic.a` or `libLiteCore.dylib` into your target.

## Linux

**Important!** LiteCore uses a couple of external libraries, which may or may not be installed in your system already. If not, please install the appropriate development packages via your package manager. You must have the following libraries present:
    
- libz
- libicu
- libpthread

You can use either g++ or clang++ for compilation but you will need to honor the minimum versions of each, and only g++ is officially supported.

- clang: 5.0+
    - libstdc++: 7.0+ **or**
    - libc++: Version from LLVM 5 or higher (unclear)
- g++: 7.0+

### Actually Building

Once you've got the dependencies and compiler installed, do this from the root directory of the source tree:

```sh
mkdir build_cmake/unix
cd build_cmake/unix

# Use whatever clang you have installed
cmake -DCMAKE_BUILD_TYPE=MinSizeRel ..

# And a reasonable number (# of cores?) for the j flag
make -j8 LiteCore
```

If CMake's initial configuration checks fail, the setup may be left in a broken state and will then fail immediately. To remedy this simply delete the `unix` directory and try again.

## Android

Android has a bit longer of a command line invocation but it is the same idea as the Linux build above.  Since Android now ships CMake and a toolchain file, the best course of action is to make use of it

- Architecture:  The architecture of the device being built for (x86, x86_64, armeabi-v7a [in example], arm64-v8a)
- Version: The minimum Android API level that the library will support (22 in the following)

```sh
# Set these appropriately for your system
export SDK_HOME=<path/to/android/sdk/root>
export NDK_VER="20.1.5948944" # Or whatever version you want
export CMAKE_VER="3.10.2.4988404" # Must be this or higher
export CMAKE_PATH="${SDK_HOME}/cmake/${CMAKE_VER}/bin" 

# Use the same name as the architecture being built for (e.g. armeabi-v7a)
mkdir -p build_cmake/android/lib/armeabi-v7a
cd build_cmake/android/lib/armeabi-v7a
${CMAKE_PATH}/cmake \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${SDK_HOME}/ndk/${NDK_VER}/build/cmake/android.toolchain.cmake" \
    -DCMAKE_MAKE_PROGRAM="${CMAKE_PATH}/ninja" \
    -DANDROID_NATIVE_API_LEVEL=19 \
    -DANDROID_ABI=armeabi-v7a \
    -DBUILD_ENTERPRISE=ON \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    ../../../..

${CMAKE_PATH}/cmake --build . --target LiteCore
```

## Windows Desktop

Open the Visual Studio 2017 Developer Command Prompt and navigate to the repo root.  Then execute:
    
    * 64-bit build *
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" -G "Visual Studio 15 2017 Win64" ..
    
    * 32-bit build *
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" -G "Visual Studio 15 2017" ..
    
This will create `LiteCore.sln` in the directory that you can open with Visual Studio.

## Windows Store

Open the Visual Studio 2015 Developer Command Prompt and navigate to the repo root.  Then execute:
    
    * x64 build *
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" -G "Visual Studio 15 2017 Win64" -DCMAKE_SYSTEM_NAME=WindowsStore
    -D CMAKE_SYSTEM_VERSION="10.0.16299.0" ..
    
    * x86 build *
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" -G "Visual Studio 15 2017" -DCMAKE_SYSTEM_NAME=WindowsStore
    -D CMAKE_SYSTEM_VERSION="10.0.16299.0" ..
    
    * ARM build *
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" -G "Visual Studio 15 2017 ARM" -DCMAKE_SYSTEM_NAME=WindowsStore
    -D CMAKE_SYSTEM_VERSION="10.0.16299.0" ..
    
This will create `LiteCore.sln` in the directory that you can open with Visual Studio.

# Documentation

## API

The C API headers are in `C/include/`. Generally you just need to include `c4.h`.

We have [online C API documentation](https://couchbase.github.io/couchbase-lite-core/C/html/modules.html), generated by Doxygen from the headers.

If this is out of date, or you want a local copy, you can generate your own by running the following commands from a shell at the root directory of the repo:

    cd C
    doxygen
    
The main page is then located at `../docs/C/html/modules.html`.

**The C API is considered unstable** and may change without notice, since it's considered an internal API of Couchbase Lite. In the future we want to provide a stable and supported C/C++ API, but not yet.

**Do not call any C++ APIS**, nor include any headers not in `C/include/` -- these are the underlying implementation beneath the C API. They are even more unstable, expose internal functionality we don't support, and may blow up if used incorrectly. The exception is `c4.hh`, which provides some handy C++ wrappers around the C API and will make your life more pleasant if you code in C++.

## Internal Implementation

For those interested in diving into the implementation, there is [an overview of the major classes](https://github.com/couchbase/couchbase-lite-core/blob/master/docs/overview/index.md).

# Current Authors

Jens Alfke ([@snej](https://github.com/snej)), Jim Borden ([@borrrden](https://github.com/borrrden))

# License

Like all Couchbase open source code, this is released under the Apache 2 [license](LICENSE).

[CBL]: http://www.couchbase.com/nosql-databases/couchbase-mobile
[CBL_C]: https://github.com/couchbaselabs/couchbase-lite-C
[N1QL]: https://www.couchbase.com/n1ql
[FLEECE]: https://github.com/couchbaselabs/fleece
[BLIP]: https://github.com/couchbase/couchbase-lite-core/blob/master/Networking/BLIP/README.md
