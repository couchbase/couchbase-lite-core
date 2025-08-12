#!/bin/bash -e

# Copyright 2017-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

set -e
shopt -s extglob dotglob

SCRIPT_DIR=`dirname $0`
pushd $SCRIPT_DIR/../..

mkdir "couchbase-lite-core"
git submodule update --recursive --init
mv !(couchbase-lite-core) couchbase-lite-core

# Sometimes a PR depends on a PR in the EE repo as well.  This needs to be convention based, so if there is a branch with the name PR-###
# (with the GH PR number) in the EE repo then use that, otherwise use the name of the target branch (master, release/XXX etc)
git clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $BRANCH_NAME --recursive --depth 1 couchbase-lite-core-EE || \
    git clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $CHANGE_TARGET --recursive --depth 1 couchbase-lite-core-EE

ulimit -c unlimited # Enable crash dumps
mkdir -p "couchbase-lite-core/build_cmake/macos"
pushd "couchbase-lite-core/build_cmake/macos"

cmake -DBUILD_ENTERPRISE=ON -DCMAKE_BUILD_TYPE=Debug -DCODE_COVERAGE_ENABLED=ON ../..
core_count=`getconf _NPROCESSORS_ONLN`
make -j `expr $core_count + 1`

# Note only for macOS
export MallocScribble=1
export MallocDebugReport=stderr

export LiteCoreTestsQuiet=1

pushd LiteCore/tests
./CppTests -r quiet
popd

pushd C/tests
./C4Tests -r quiet
popd

ARCH=$(lipo -info libLiteCore.dylib | awk '{print $NF}')

mkdir -p coverage_reports
xcrun llvm-profdata merge -sparse LiteCore/tests/default.profraw C/tests/default.profraw -o AllTests.profdata
xcrun llvm-cov show -instr-profile=AllTests.profdata -show-line-counts-or-regions -arch $ARCH -output-dir=$PWD/coverage_reports -format="html" \
  -ignore-filename-regex="/vendor/SQLiteCpp/*" -ignore-filename-regex="vendor/sockpp/*" -ignore-filename-regex="vendor/fleece/ObjC/*" \
  -ignore-filename-regex="vendor/fleece/vendor/*" -ignore-filename-regex="Networking/WebSockets/*" -ignore-filename-regex="C/c4DocEnumerator.cc" \
  -ignore-filename-regex="LiteCore/Query/N1QL_Parser/*" -ignore-filename-regex="*sqlite3*c" -ignore-filename-regex="*.leg" \
  -ignore-filename-regex="vendor/mbedtls/*" -ignore-filename-regex="vendor/sqlite3-unicodesn" -ignore-filename-regex="vendor/fleece/Fleece/Integration/ObjC/*" \
  -ignore-filename-regex="EE/Encryption/*" \
  libLiteCore.dylib

if [ "$1" == "--show-results" ]; then
  open coverage_reports/index.html
elif [ "$1" == "--export-results" ]; then
  xcrun llvm-cov export -instr-profile=AllTests.profdata -arch $ARCH \
    -ignore-filename-regex="/vendor/SQLiteCpp/*" -ignore-filename-regex="vendor/sockpp/*" -ignore-filename-regex="vendor/fleece/ObjC/*" \
    -ignore-filename-regex="vendor/fleece/vendor/*" -ignore-filename-regex="Networking/WebSockets/*" -ignore-filename-regex="C/c4DocEnumerator.cc" \
    -ignore-filename-regex="LiteCore/Query/N1QL_Parser/*" -ignore-filename-regex="*sqlite3*c" -ignore-filename-regex="*.leg" \
    -ignore-filename-regex="vendor/mbedtls/*" -ignore-filename-regex="vendor/sqlite3-unicodesn" -ignore-filename-regex="vendor/fleece/Fleece/Integration/ObjC/*" \
    -ignore-filename-regex="EE/Encryption/*" \
    libLiteCore.dylib > output.json

    if [[ "$2" == "--push" ]] && [[ -n "$CHANGE_ID" ]]; then
      python3 -m venv venv
      source venv/bin/activate 
      pip install -r ../scripts/push_coverage_results_requirements.txt
      python ../scripts/push_coverage_results.py -r ./output.json -n $CHANGE_ID
    fi
fi

popd
popd
