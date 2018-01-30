#!/bin/bash -e

SCRIPT_DIR=`dirname $0`
pushd $SCRIPT_DIR/..

mkdir -p macos
pushd macos
core_count=`getconf _NPROCESSORS_ONLN`
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ../..
make -j `expr $core_count + 1`
if [-d "couchbase-lite-core" ]; then
    dysumutil couchbase-lite-core/libLiteCore.dylib couchbase-lite-core/libLiteCore.dylib.dSYM
    strip -x couchbase-lite-core/libLiteCore.dylib
else
    dsymutil libLiteCore.dylib -o libLiteCore.dylib.dSYM
    strip -x libLiteCore.dylib
fi
popd
popd
