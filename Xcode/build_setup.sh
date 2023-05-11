#! /bin/bash -e

# Copyright 2019-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

# This script does some setup before using CMake to build a library.
# It's used by build_mbedtls.sh.

# It takes one argument, the name to use for the CMake build output directory.

# Create the CMake build directory:
if [[ "$PER_VARIANT_OBJECT_FILE_DIR" != "" ]]
then
    CMAKE_BUILD_DIR="$PER_VARIANT_OBJECT_FILE_DIR/$1"
else
    CMAKE_BUILD_DIR="$OBJECT_FILE_DIR/$1"
fi

mkdir -p "$CMAKE_BUILD_DIR"
cd "$CMAKE_BUILD_DIR"

# What architectures to build for Mac?
if [[ "$ARCHS" =~ "arm64" ]]
then
    MAC_ARCHS="x86_64;arm64"
else
    MAC_ARCHS="x86_64"
fi

# Define an initial CMAKE_OPTS containing the CMake option flags.
# The main build script should then append to this and pass it to `cmake`.
if [[ "$PLATFORM_NAME" == "iphoneos" ]]
then
    # CMake options for iOS device -- use ios.toolchain.cmake
    CMAKE_OPTS="$CMAKE_OPTS \
                -DCMAKE_TOOLCHAIN_FILE=$SRCROOT/../vendor/ios-cmake/ios.toolchain.cmake \
                -DPLATFORM=OS \
                -DDEPLOYMENT_TARGET=$IPHONEOS_DEPLOYMENT_TARGET" \
                -DENABLE_BITCODE=0"
elif [[ "$PLATFORM_NAME" == "iphonesimulator" ]]
then
    # CMake options for iOS Simulator -- use ios.toolchain.cmake
    CMAKE_OPTS="$CMAKE_OPTS \
                -DCMAKE_TOOLCHAIN_FILE=$SRCROOT/../vendor/ios-cmake/ios.toolchain.cmake \
                -DPLATFORM=SIMULATOR -DARCHS=$MAC_ARCHS;i386\
                -DDEPLOYMENT_TARGET=$IPHONEOS_DEPLOYMENT_TARGET"
    # TODO: Delete ';i386' above when we drop 32-bit iOS support
elif [[ "$PLATFORM_NAME" == "macosx" ]]
then
    # CMake options for macOS
    CMAKE_OPTS="$CMAKE_OPTS \
                -DCMAKE_OSX_ARCHITECTURES=$MAC_ARCHS"
fi
