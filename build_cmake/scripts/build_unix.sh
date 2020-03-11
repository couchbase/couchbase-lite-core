#!/bin/bash

set -e

SCRIPT_DIR=`dirname $0`
cd $SCRIPT_DIR/..
mkdir -p unix
cd unix
core_count=`getconf _NPROCESSORS_ONLN`

cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ../..
make -j `expr $core_count + 1` LiteCore
