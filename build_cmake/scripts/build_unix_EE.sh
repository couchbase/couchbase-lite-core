#!/bin/bash

set -e

SCRIPT_DIR=`dirname $0`
cd $SCRIPT_DIR/..
mkdir -p unix_EE
cd unix_EE
core_count=`getconf _NPROCESSORS_ONLN`

cmake -DBUILD_ENTERPRISE=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo ../..
make -j `expr $core_count + 1`
