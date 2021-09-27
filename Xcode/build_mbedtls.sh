#! /bin/bash -e

# Copyright 2019-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

# This script builds the mbedTLS submodule.
# It's run by Xcode when building the "mbedTLS" target.

source "$SRCROOT/build_setup.sh" mbedtls

# Set up the CMake build options:
CMAKE_OPTS="$CMAKE_OPTS \
            -DENABLE_PROGRAMS=0 \
            -DENABLE_TESTING=0"

if [[ "$CONFIGURATION" == "Release*" ]]
then
    CMAKE_OPTS="$CMAKE_OPTS -DCMAKE_BUILD_TYPE=RelWithDebInfo"
else
    CMAKE_OPTS="$CMAKE_OPTS -DCMAKE_BUILD_TYPE=Debug"
    if [ "$ENABLE_ADDRESS_SANITIZER" == "YES" ]
    then
        CMAKE_OPTS="$CMAKE_OPTS -DLWS_WITH_ASAN=1"
    fi
fi

echo "CMake options: $CMAKE_OPTS"

# Build!
if [[ "$EFFECTIVE_PLATFORM_NAME" == "-maccatalyst" ]]
then
    cmake "$SRCROOT/../vendor/mbedtls" $CMAKE_OPTS \
        '-DCMAKE_CXX_FLAGS=-target x86_64-apple-ios13.1-macabi' \
        '-DCMAKE_C_FLAGS=-target x86_64-apple-ios13.1-macabi'
else
    cmake "$SRCROOT/../vendor/mbedtls" $CMAKE_OPTS
fi
make

# Copy the resulting static libraries to the Xcode build dir where the linker will find them:
mkdir -p "$BUILT_PRODUCTS_DIR"
cp -pv library/libmbed*.a "$BUILT_PRODUCTS_DIR/"
cp -pv crypto/library/libmbed*.a "$BUILT_PRODUCTS_DIR/"
