#!/bin/bash

SCRIPT_DIR=`dirname $0`
pushd $SCRIPT_DIR/..

mkdir -p unix
pushd unix
core_count=`getconf _NPROCESSORS_ONLN`
CC=clang CXX=clang++ cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLITECORE_BUILD_SQLITE=1 ../..
make -j `expr $core_count + 1`
if [ -d "couchbase-lite-core" ]; then
    # Enterprise Edition
    ../scripts/strip.sh `pwd`/couchbase-lite-core
else
    ../scripts/strip.sh `pwd`
fi
popd
popd
