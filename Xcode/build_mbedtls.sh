#! /bin/bash -e
# This script builds the mbedTLS submodule.
# It's run by Xcode when building the target LiteCoreWebSocket.

cd "$SRCROOT/../vendor/mbedtls"
mkdir -p build
cd build
cmake .. -DENABLE_PROGRAMS=0 -DENABLE_TESTING=0
make
