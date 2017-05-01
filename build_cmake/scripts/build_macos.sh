#!/bin/bash -e

SCRIPT_DIR=`dirname $0`
pushd $SCRIPT_DIR/..

mkdir -p macos
pushd macos
core_count=`getconf _NPROCESSORS_ONLN`
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ../..
make -j `expr $core_count + 1`
popd
popd
