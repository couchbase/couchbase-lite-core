#!/bin/bash

SCRIPT_DIR=`dirname $0`
pushd $SCRIPT_DIR/..

mkdir unix
pushd unix
core_count=`getconf _NPROCESSORS_ONLN`
CC=clang CXX=clang++ cmake -DBLIP_NO_FRAMING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo ../..
make -j `expr $core_count + 1`
../scripts/strip.sh
popd
popd
