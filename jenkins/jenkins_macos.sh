#!/bin/bash -e

# Copyright 2017-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

SCRIPT_DIR=`dirname $0`
pushd $SCRIPT_DIR/..
pushd ..
git submodule update --recursive --init
popd

mkdir -p macos
pushd macos
cmake -DCMAKE_BUILD_TYPE=Debug ../..
core_count=`getconf _NPROCESSORS_ONLN`
make -j `expr $core_count + 1`

pushd LiteCore/tests
./CppTests "[EE]" -r quiet
popd

pushd C/tests
./C4Tests "[EE]" -r quiet
