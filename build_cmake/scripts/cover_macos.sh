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

mkdir -p macos
pushd macos
cmake -DCMAKE_BUILD_TYPE=Debug -DCODE_COVERAGE_ENABLED=ON ../..
core_count=`getconf _NPROCESSORS_ONLN`
make -j `expr $core_count + 1`

pushd LiteCore/tests
./CppTests -r quiet
popd

pushd C/tests
./C4Tests -r quiet
popd

mkdir -p coverage_reports
xcrun llvm-profdata merge -sparse LiteCore/tests/default.profraw C/tests/default.profraw -o AllTests.profdata
xcrun llvm-cov show -instr-profile=AllTests.profdata -show-line-counts-or-regions -arch x86_64 -output-dir=$PWD/coverage_reports -format="html" \
  -ignore-filename-regex="/vendor/SQLiteCpp/*" -ignore-filename-regex="vendor/sockpp/*" -ignore-filename-regex="vendor/fleece/ObjC/*" \
  -ignore-filename-regex="vendor/fleece/vendor/*" -ignore-filename-regex="Networking/WebSockets/*" -ignore-filename-regex="C/c4DocEnumerator.cc" \
  -ignore-filename-regex="LiteCore/Query/N1QL_Parser/*" -ignore-filename-regex="*sqlite3*c" -ignore-filename-regex="*.leg" \
  -ignore-filename-regex="vendor/mbedtls/*" -ignore-filename-regex="vendor/sqlite3-unicodesn" -ignore-filename-regex="vendor/fleece/Fleece/Integration/ObjC/*" \
  libLiteCore.dylib

if [ "$1" == "--show-results" ]; then
  open coverage_reports/index.html
fi

popd
popd
