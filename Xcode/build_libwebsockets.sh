#! /bin/bash -e

# Copyright 2019-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

# This script builds the libwebsockets submodule.
# It's run by Xcode when building the target LiteCoreWebSocket.

source "$SRCROOT/build_setup.sh" libwebsockets

# Set up the CMake build options:
CMAKE_OPTS="$CMAKE_OPTS \
           -DLWS_WITHOUT_SERVER=0 \
           -DLWS_WITH_HTTP2=0 \
           -DLWS_WITH_SHARED=0 \
           -DLWS_WITH_LEJP=0 \
           -DLWS_WITH_STRUCT_JSON=0 \
           -DLWS_WITHOUT_TESTAPPS=1 \
           -DLWS_WITHOUT_TEST_PING=1 \
           -DLWS_WITHOUT_TEST_CLIENT=1 \
           -DLWS_WITHOUT_TEST_SERVER=1 \
           -DLWS_WITHOUT_TEST_SERVER_EXTPOLL=1 \
           -DLWS_WITH_MBEDTLS=1 \
           -DCMAKE_C_FLAGS=-DCONFIG_OPENSSL_DEBUG \
           -DLWS_MBEDTLS_INCLUDE_DIRS=../mbedtls/include \
           -DMBEDCRYPTO_LIBRARY=../mbedtls/library/libmbedcrypto.a \
           -DMBEDTLS_LIBRARY=../mbedtls/library/libmbedtls.a \
           -DMBEDX509_LIBRARY=../mbedtls/library/libmbedx509.a"

if [[ "$CONFIGURATION" == "Release*" ]]
then
    CMAKE_OPTS="$CMAKE_OPTS -DCMAKE_BUILD_TYPE=RelWithDebugInfo"
else
    CMAKE_OPTS="$CMAKE_OPTS -DCMAKE_BUILD_TYPE=Debug"
    if [ "$ENABLE_ADDRESS_SANITIZER" == "YES" ]
    then
        CMAKE_OPTS="$CMAKE_OPTS -DLWS_WITH_ASAN=1"
    fi
fi

echo "CMake options: $CMAKE_OPTS"

# Build!
cmake "$SRCROOT/../vendor/libwebsockets" $CMAKE_OPTS
make

# Copy the resulting static library to the Xcode build dir where the linker will find it:
mkdir -p "$TARGET_BUILD_DIR"
cp lib/libwebsockets.a "$TARGET_BUILD_DIR/"

# Copy the generated lws_config.h to the derived sources dir where #include will find it,
# but only if it's changed, to avoid triggering a bunch of recompilation:
if cmp --quiet include/lws_config.h "$DERIVED_SOURCES_DIR/lws_config.h"
then
    echo "No change in lws_config.h"
else
    echo "Copying lws_config.h to $DERIVED_SOURCES_DIR"
    mkdir -p "$DERIVED_SOURCES_DIR"
    cp include/lws_config.h "$DERIVED_SOURCES_DIR/lws_config.h"
fi
