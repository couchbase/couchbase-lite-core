**Couchbase Lite Core** (aka **LiteCore**) is the core engine for [Couchbase Lite][CBL]. It provides a cross-platform implementation of the database CRUD and query features, document versioning, and replication/sync.

All platform implementations of Couchbase Lite are built atop this core, adding higher-level language & platform bindings.

> [!CAUTION]
> **We do _not_ recommend (or support) using LiteCore directly in other projects.** Its API is unstable and can be tricky to use. Instead, use [Couchbase Lite for C][CBL_C], a cross-platform version of Couchbase Lite with a C (and C++) API.

# Platform Support

LiteCore runs on Mac OS, iOS, Android, various other flavors of Unix, and Windows.

It is written in C++ (using C++20 features) and compiles with Clang, G++ and MSVC.

# Branches and CI

* Active development usually happens on the `master` branch, which may therefore be temporarily broken.
* There are various `release` branches (prefixed with `release/`, *except* for `release/master` which will be in the next point) which track along with releases of Couchbase Lite in the following manner until 3.1.x, which will start using `release/x.y`:
  * `release/iridium` : 2.5.x
  * `release/cobalt` : 2.6.x
  * `release/mercury` : 2.7.x
  * `release/hydrogen` : 2.8.x
  * `release/lithium` : 3.0.x
* `release/master` tracks the latest stable master commit (with integration tests into Couchbase Lite), and `staging/master` is a place for candidates for a stable master build.
* PR validation ensures that things keep building and passing tests (where possible) on all supported platforms.
* GitHub CI builds the Community Edition (CE). The "continuous-integration/jenkins/pr-head" check is an external one that runs on our Jenkins servers. This is actually multiple checks that build the Enterprise Edition (EE).
* CMake is available and used for all platforms except for iOS. The CLion IDE is supported. There is also an Xcode project in the `Xcode` folder.

# Building It

> [!CAUTION]
> Again, we do _not_ recommend (or support) using LiteCore directly in other projects. Its API is unstable and can be tricky to use. (Instead see [Couchbase Lite for C][CBL_C], a cross-platform version of Couchbase Lite with a C (and C++) API.) The build instructions here are for the benefit of developers who want to debug or extend Couchbase Lite at the LiteCore level.

> [!IMPORTANT]
> This repo has **submodules**. Make sure they're checked out. Either use `git clone --recursive` to download LiteCore, or else after the clone run `git submodule update --init --recursive`.

Building the Enterprise Edition (EE) requires the separate `couchbase-lite-core-EE` repo, which is private and available only to Couchbase employees. This must be checked out _next to_ `couchbase-lite-core` so that relative paths between them work correctly.

Once you've cloned or downloaded the source tree...

## macOS, iOS

### With CLion:

* Open the repo's root directory with CLion.
* Choose CMake as the build system if asked.
* CLion should run the CMake scripts.
* Select the appropriate CMake profile, usually "Debug CE", from the menu at the top of the window.
* Choose "CppTests" or "C4Tests" from the configurations menu at the top of the window.
* Choose _Run>Run CppTests_.

### With Xcode

> [!TIP]
> Xcode's UI is pretty flaky with large C++ projects like this. It builds correctly, but you may see errors that don't exist, especially if you're building EE. CLion is more stable.

* Make sure you have Xcode **15** or later.
* Open **Xcode/LiteCore.xcodeproj**. 
* Select the scheme **LiteCore static** or **LiteCore dylib**. 
* Choose _Product>Build_ (for a debug build) or _Product>Build For>Profiling_ (for a release/optimized build).
* To run unit tests, choose the scheme **CppTests** and run it. Then run **C4Tests** too.

## Linux

> [!IMPORTANT]
> LiteCore uses a couple of external libraries, which may or may not be installed in your system already. If not, please install the appropriate development packages via your package manager. You must have the following libraries present:
    
- libz
- libicu
- libpthread

You can use either g++ or clang++ for compilation but you will need to honor the minimum versions of each, and only g++ is officially supported.

- clang: 15.0+
- g++: 11.0+

On Ubuntu or Debian you can run e.g.

```sh
sudo apt-get install cmake gcc-11 g++-11 libicu-dev zlib1g-dev
```

and prefix the `cmake` line (below) with `CC=/usr/bin/gcc-11 CXX=/usr/bin/c++-11`

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

> [!TIP]
> If you encounter a failure in one of the Fleece encoder tests, it's likely because your system doesn't have the French locale installed. Run `sudo localedef -v -c -i fr_FR -f UTF-8 fr_FR`.

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

# Working On LiteCore

There are several [overview documentation files][DOCS] in the repo, and more in the [LiteCore wiki][WIKI].

If you use `git blame`, you may find it useful to run:
```shell
git config blame.ignoreRevsFile .git-blame-ignore-revs
```
This will ignore any commits that we have marked as such in that file, such as ones that make global formatting changes.

## Unit Tests

We use the Catch2 framework for unit tests. If it's new to you, you can pick up the basics by looking at existing tests, but you might also want to [RTFM][CATCH2]. We also have a bunch of test-related utilities in `c4Test.hh` and `TestsCommon.hh`.

> [!TIP]
> To run a single test, add its name to the command line. Or to run all tests with a particular tag, add the tag name in square brackets. In Xcode, you can edit the command line in the scheme editor's "Arguments" tab. In CLion you can just click the green arrow in the left margin next to the test's first line.

## Development Workflow

- Create a branch from the appropriate source branch, such as `master` or a release branch. If appropriate, use a name prefix like `fix/` for bug fixes or `feature/` for new functionality. If there is a JIRA ticket associated with this work, it's customary to put its ID in the branch name.
- Make your changes. Add or modify unit tests if appropriate.
- Making WIP commits and pushing to GitHub periodically is a good way to back up your work!
- Run both CppTests and C4Tests in debug mode; both must pass. (Note: we sometimes have flaky tests. If a test unrelated to your changes fails, ask on the #mobile-litecore Slack channel.)
- If your PR creates, renames or moves `.cc` files, both the CMake scripts _and_ the Xcode project need to be updated. CMake usually doesn't care about `.hh` files, but Xcode does. Remember to update whichever build system you didn't use, or CI will report build errors! If you need help, ask on #mobile-litecore.
- Run `./build_cmake/scripts/run-clang-format.sh` to reformat your changes.
- Commit the changes. If you've got intermediate WIP commits, flatten them into one with interactive rebase. (Multiple commits are OK if you've made several changes that might be easier to review separately.) Make the commit title and message descriptive, and append the Jira ticket ID if any to the title.
- If by now there are newer commits on the parent branch, pull them in 'rebase' mode; then reformat, rebuild and re-test.
- Push your branch to GitHub and create a pull request. Please make the PR title and description, um, descriptive. Include the Jira ticket ID.
- If there's a Jira ticket, add a comment with a link to the PR.
- Wait for the CI checks to finish.
  - You did remember to run clang-format, right?
  - If a platform build fails, drill into the logs to see what went wrong. Searching for "error" or "error:" is often useful.
  - The "continuous-integration/jenkins/pr-head" check is an external one that runs on our Jenkins servers. This is actually multiple checks that build Enterprise Edition (EE). The Jenkins page will show a flowchart at the top with red for failed builds; click on those to show their logs. If a problem appears here but not in the GitHub CI checks, it might be specific to EE builds, i.e. code inside `#ifdef COUCHBASE_ENTERPRISE` blocks.
  - Many compile errors are caused by missing `#include`s; the various C++ library implementations vary in which headers include which other ones, so sometimes your local compiler lets you get away without explicitly including a header you need, but another compiler won't.
  - Other build errors may be caused by warnings from a different compiler. In general, Clang is the strictest because we've turned on nearly all warnings and `-Werror`. Hopefully the warning will make sense.
- Once CI is green, request a code review from an appropriate senior engineer.
- When the PR is approved, rebase & merge it!

# Current Authors

Jens Alfke ([@snej](https://github.com/snej)), Jim Borden ([@borrrden](https://github.com/borrrden)), Jianmin Zhao ([@jianminzhao](https://github.com/jianminzhao))

# License

The source code in this repo is governed by the [BSL 1.1](LICENSE.txt) license.

[CBL]: http://www.couchbase.com/nosql-databases/couchbase-mobile
[CBL_C]: https://github.com/couchbaselabs/couchbase-lite-C
[N1QL]: https://www.couchbase.com/n1ql
[FLEECE]: https://github.com/couchbaselabs/fleece
[BLIP]: https://github.com/couchbase/couchbase-lite-core/blob/master/Networking/BLIP/README.md
[DOCS]: https://github.com/couchbase/couchbase-lite-core/blob/master/docs/overview/index.md
[WIKI]: https://github.com/couchbase/couchbase-lite-core/wiki
[CATCH2]: https://catch2-temp.readthedocs.io/en/latest/
