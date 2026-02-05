**Couchbase Lite Core** (aka **LiteCore**) is the core storage and query engine for [Couchbase Lite][CBL]. It provides a cross-platform implementation of the database CRUD and query features, document versioning, and replication/sync.

All platform implementations of Couchbase Lite (from 2.0 onward) are built atop this core, adding higher-level language & platform bindings.

**IMPORTANT:** We do _not_ recommend (or support) using LiteCore directly in other projects. Its API is unstable and can be tricky to use. Instead, use [Couchbase Lite for C][CBL_C], a cross-platform version of Couchbase Lite with a C (and C++) API.

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
    * JSON query syntax, similar to a parse tree; easy to generate from platform APIs like NSPredicate
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
* C and C++ APIs (rather low-level; not considered "public" APIs yet.)
* Bindings to C# and Java (via Couchbase Lite)

# Platform Support

LiteCore runs on Mac OS, iOS, Android, various other flavors of Unix, and Windows.

It is written in C++ (using C++20 features) and compiles with Clang, G++ and MSVC.

It has been experimentally built and run on the Raspberry Pi but this is not an actively maintained use.

# Status

**As of June 2020:** LiteCore is in active use as the engine of Couchbase Lite 2! Development continues...

* Active development usually happens on the `master` branch, which may therefore be temporarily broken.
* There are various `release` branches (prefixed with `release/`, *except* for `release/master` which will be in the next point) which track along with releases of Couchbase Lite in the following manner until 3.1.x, which will start using `release/x.y`:
  * `release/iridium` : 2.5.x
  * `release/cobalt` : 2.6.x
  * `release/mercury` : 2.7.x
  * `release/hydrogen` : 2.8.x
  * `release/lithium` : 3.0.x
* `release/master` tracks the latest stable master commit (with integration tests into Couchbase Lite), and `staging/master` is a place for candidates for a stable master build.
* PR validation ensures that things keep building and passing tests (where possible) on all supported platforms.
* CMake is available and used for all platforms except for iOS.  There is also an Xcode project that is independently maintained in the `Xcode` folder.

# Building It

We do _not_ recommend (or support) using LiteCore directly in other projects. Its API is unstable and can be tricky to use. (Instead see [Couchbase Lite for C][CBL_C], a cross-platform version of Couchbase Lite with a C (and C++) API.) The build instructions here are for the benefit of developers who want to debug or extend Couchbase Lite at the LiteCore level.

**Very Important:**

* This repo has **submodules**. Make sure they're checked out. Either use `git clone --recursive` to download LiteCore, or else after the clone run `git submodule update --init --recursive`.

Once you've cloned or downloaded the source tree...

## macOS, iOS

If you want to use Objective-C or Swift APIs, you should use Couchbase Lite instead — check out and build the [couchbase-lite-ios][CBL_iOS_2] repo, which itself includes LiteCore as a submodule.

The following instructions are to build just LiteCore on its own:

* Make sure you have Xcode **15** or later.
* Open **Xcode/LiteCore.xcodeproj**. 
* Select the scheme **LiteCore static** or **LiteCore dylib**. 
* Choose _Product>Build_ (for a debug build) or _Product>Build For>Profiling_ (for a release/optimized build).
* Link the build product `libLiteCoreStatic.a` or `libLiteCore.dylib` into your target.

### Testing

In the Xcode project, choose the scheme **CppTests** and run it. Then run **C4Tests** too.

## Linux

**Important!** LiteCore uses a couple of external libraries, which may or may not be installed in your system already. If not, please install the appropriate development packages via your package manager. You must have the following libraries present:
    
- libz
- libicu
- libpthread

You can use either g++ or clang++ for compilation but you will need to honor the minimum versions of each, and only clang is officially supported.

- clang: 15.0+
- g++: 11.0+

On Ubuntu or Debian you can run e.g.

```sh
sudo apt-get install cmake clang-15 libicu-dev zlib1g-dev
```

and prefix the `cmake` line (below) with `CC=/usr/bin/clang-15 CXX=/usr/bin/clang++-15`

### Actually Building

Once you've got the dependencies and compiler installed, do this from the root directory of the source tree (works for both macOS and Linux):

```sh
mkdir build_cmake/unix
cd build_cmake/unix

# Use whatever compiler you have installed
cmake -DCMAKE_BUILD_TYPE=MinSizeRel ../..

# And a reasonable number (# of cores?) for the j flag
make -j8 LiteCore
```

If CMake's initial configuration checks fail, the setup may be left in a broken state and will then fail immediately. To remedy this simply delete the `unix` directory and try again.

### Testing

To run tests you'll want to use the Debug build type and enable the GCC address and undefined-behavior sanitizers. For complicated reasons we have two test binaries, called CppTests and C4Tests; the first tests internals and the second tests the API exported from the shared library.

```sh
mkdir build_cmake/unix_tests
cd build_cmake/unix_tests

cmake -DCMAKE_BUILD_TYPE=Debug -DLITECORE_SANITIZE=ON ../..
make CppTests
make C4Tests

(cd LiteCore/tests && ./CppTests -r quiet)
(cd C/tests && ./C4Tests -r quiet)
```

> Note: If you encounter a failure in one of the Fleece encoder tests, it's likely because your system doesn't have the French locale installed. Run `sudo localedef -v -c -i fr_FR -f UTF-8 fr_FR`.

## Android

Android has a bit longer of a command line invocation but it is the same idea as the Linux build above.  The current stance of Google is that CMake support for Android should be a part of the main CMake downstream now, which is a departure from the previous stance that Google would fork and maintain their own version of CMake which they then distributed.  Similar story for the Ninja build system that Google favors.

- Architecture:  The architecture of the device being built for (x86, x86_64, armeabi-v7a [in example], arm64-v8a)
- Version: The minimum Android API level that the library will support (22 in the following)

CMake must be 3.23 or higher for this technique.

```sh
# Set these appropriately for your system
export SDK_HOME=<path/to/android/sdk/root>
export NDK_VER="23.1.7779620" # Or whatever version you want, but if you go too much older you need to use a different technique

# Use the same name as the architecture being built for (e.g. armeabi-v7a)
mkdir -p build_cmake/android/lib/armeabi-v7a
cd build_cmake/android/lib/armeabi-v7a
cmake \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${SDK_HOME}/ndk/${NDK_VER}/build/cmake/android.toolchain.cmake" \
    -DCMAKE_MAKE_PROGRAM="ninja" \
    -DANDROID_NATIVE_API_LEVEL=22 \
    -DANDROID_ABI=armeabi-v7a \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    ../../../..

ninja LiteCore
```

## Windows Desktop

Open the Visual Studio 2022 Developer Command Prompt and navigate to the repo root.  Then execute:
    
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" -G "Visual Studio 17 2022" -A x64 ..
    
This will create `LiteCore.sln` in the directory that you can open with Visual Studio.  Note that only 64-bit x86 is supported now, but 32-bit x86 is still buildable.

## Windows Store

Open the Visual Studio 2022 Developer Command Prompt and navigate to the repo root.  Then execute:
    
    cd build_make
    "C:\Program Files (x86)\CMake\bin\cmake.exe" -G "Visual Studio 17 2022" -A x64 -DCMAKE_SYSTEM_NAME=WindowsStore
    -D CMAKE_SYSTEM_VERSION="10.0"
    -DCMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION="10.0.19041.0" ..
    
This will create `LiteCore.sln` in the directory that you can open with Visual Studio.  Swap `x64` with `ARM64` in the above to get a 64-bit ARM build.  `Win32` and `ARM` will also build but are no longer supported.  

# Git Blame

If you are inspecting the Blame of this project, you may find it useful to run:
```shell
git config blame.ignoreRevsFile .git-blame-ignore-revs
```
This will ignore any commits that we have marked as such in that file (formatting, etc.)

# Documentation

## API

The C API headers are in `C/include/`. Generally you just need to include `c4.h`.

We have [online C API documentation](https://couchbase.github.io/couchbase-lite-core/C/html/modules.html), generated by Doxygen from the headers.

If this is out of date, or you want a local copy, you can generate your own by running the following commands from a shell at the root directory of the repo:

    cd C
    doxygen
    
The main page is then located at `../docs/C/html/modules.html`.

**The C API is considered unstable** and may change without notice, since it's considered an internal API of Couchbase Lite.

**Do not call any C++ APIS**, nor include any headers not in `C/include/` -- these are the underlying implementation beneath the C API. They are even more unstable, expose internal functionality we don't support, and may blow up if used incorrectly. The exception is `c4.hh`, which provides some handy C++ wrappers around the C API and will make your life more pleasant if you code in C++.

## Internal Implementation

For those interested in diving into the implementation, there is [an overview of the major classes](https://github.com/couchbase/couchbase-lite-core/blob/master/docs/overview/index.md).

# Current Authors

Jens Alfke ([@snej](https://github.com/snej)), Jim Borden ([@borrrden](https://github.com/borrrden)), Jianmin Zhao ([@jianminzhao](https://github.com/jianminzhao))

# License

The source code in this repo is governed by the [BSL 1.1](LICENSE.txt) license.

[CBL]: http://www.couchbase.com/nosql-databases/couchbase-mobile
[CBL_C]: https://github.com/couchbaselabs/couchbase-lite-C
[N1QL]: https://www.couchbase.com/n1ql
[FLEECE]: https://github.com/couchbaselabs/fleece
[BLIP]: https://github.com/couchbase/couchbase-lite-core/blob/master/Networking/BLIP/README.md
