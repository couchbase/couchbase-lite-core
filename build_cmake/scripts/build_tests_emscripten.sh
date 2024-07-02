#!/usr/bin/env bash

set -e

rm -rf build
mkdir -p build
cd build

emcmake cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..

core_count=`getconf _NPROCESSORS_ONLN`
emmake make C4Tests -j `expr $core_count + 1`
emmake make CppTests -j `expr $core_count + 1`
