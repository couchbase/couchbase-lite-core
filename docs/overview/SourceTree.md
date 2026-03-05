# LiteCore Source Tree Overview

## Source Code

- `C` -- Public API
  - `Cpp_include` -- API C++ headers
  - `include` -- API C headers
  - `scripts` -- Master list of exported symbols, and Python script to generate export lists
  - `tests` -- Unit tests
- `Crypto` -- Certificates, key-pairs, encryption, digests
- `LiteCore` -- Core implementation
    - `Android` -- Android-specific utilities
    - `BlobStore` -- Blob/attachment storage
    - `Database` -- Implementation of `C4Database` and `C4Collection`, and database- and document-related classes
    - `Logging` -- Logging
    - `Query` -- Query and index implementation
        - `N1QL_Parser` -- N1QL grammar and transpiler to JSON
        - `Translator` -- JSON-to-SQL transpiler
        - `RevTrees` -- Implementation of revision trees and version vectors
        - `Storage` -- Low-level database code: `DataFile`, `KeyStore`, `Record`, etc.
        - `Support` -- Random utilities
        - `tests` -- Unit tests
        - `Unix` -- Linux-specific utilities
- `MSVC` -- Glue code for some POSIX functions on Windows
- `Networking` -- Networking and WebSockets
  - `BLIP` -- BLIP protocol implementation
  - `HTTP` -- Low-level HTTP support, like header parsing and cookie management
  - `tests` -- Unit tests
  - `WebSockets` -- WebSocket implementation
- `Replicator` -- The replicator
  - `tests` -- Unit tests
- `REST` -- HTTP request/response handlers
- `tool_support` -- A CLI tool framework used by Edge Server and the cblite tool
- `vendor` -- Submodules
  - `fleece` -- Our binary data encoding library, plus lots of support code like RefCounted
  - `ios-cmake` -- CMake toolchain files for building for iOS _(3rd party)_
  - `linenoise-mob` -- CLI input line reader library used by tool_support _(3rd party)_
  - `mbedtls` -- TLS and crypto implementation _(3rd party)_
  - `sockpp` -- C++ sockets library _(3rd party)_
  - `sqlite3-unicodesn` -- Unicode tokenizer for SQLite FTS _(3rd party)_
  - `SQLiteCpp` -- C++ SQLite wrapper library _(3rd party)_
  - `vector_search` -- Some glue code from our vector search library
  - `zlib` -- gzip compression library _(3rd party)_

## Other Stuff

- `.github` -- GitHub CI configuration
- `.idea` -- CLion IDE config files
- `build_cmake` -- Default CMake build directory
    - `scripts` -- Shell scripts to invoke CMake
- `docs` -- API and overview documentation
- `cmake` -- CMake scripts used by top-level CMakeLists.txt
- `jenkins` -- Build scripts run by Jenkins CI
- `licenses` -- Copies of Apache and BSL licenses
- `tools` -- Scripts for downloading a prebuilt version of LiteCore
- `Xcode` -- Xcode project and associated config files
