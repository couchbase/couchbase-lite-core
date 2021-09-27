#!/bin/bash -ex

# Copyright 2020-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

script_dir=`dirname $0`
cmake_dir=$script_dir/../build_cmake/valgrind
mkdir -p $cmake_dir

if [[ -z "${WORKSPACE}" ]]; then
    WORKSPACE=`pwd`
fi

pushd $cmake_dir
cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_ENTERPRISE=ON ../..
make -j8 C4Tests

valgrind --leak-check=yes C/tests/C4Tests -r list 2>&1 | tee $WORKSPACE/memcheck.txt
valgrind --tool=drd C/tests/C4Tests -r list 2>&1 | tee $WORKSPACE/drd.txt