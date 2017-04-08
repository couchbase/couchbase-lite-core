#!/bin/bash

SCRIPT_DIR=`dirname $0`
pushd $SCRIPT_DIR/..

mkdir macos
pushd macos
core_count=`getconf _NPROCESSORS_ONLN`
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBLIP_NO_FRAMING=ON ../..
make -j `expr $core_count + 1` 
popd
popd
