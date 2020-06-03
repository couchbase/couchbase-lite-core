#!/bin/bash -e

SCRIPT_DIR=`dirname $0`
pushd $SCRIPT_DIR/..

mkdir -p macos
pushd macos
cmake -DCMAKE_BUILD_TYPE=Debug -DCODE_COVERAGE_ENABLED=ON ../..
core_count=`getconf _NPROCESSORS_ONLN`
make -j `expr $core_count + 1`

pushd LiteCore/tests
LiteCoreTestsQuiet=1 ./CppTests -r list
popd

lcov -d CMakeFiles/LiteCoreStatic.dir/ -d vendor/fleece -c -o CppTests.info
find . -type f -name '*.gcda' -delete

pushd C/tests
LiteCoreTestsQuiet=1 ./C4Tests -r list
popd

lcov -d CMakeFiles/LiteCoreStatic.dir/ -d vendor/fleece -c -o C4Tests.info
find . -type f -name '*.gcda' -delete

lcov -a CppTests.info -a C4Tests.info -o AllTests.info
lcov --remove AllTests.info '/usr/include/*' '/System/*' '/Applications/*' '*/vendor/SQLiteCpp/*' '*/vendor/sockpp/*' '*/vendor/fleece/ObjC/*' '*/vendor/fleece/vendor/*' '*/Networking/WebSockets/*' '*/C/c4DocEnumerator.cc' '*/LiteCore/Query/N1QL_Parser/*' '*sqlite3*c' '*.leg' -o AllTests_Filtered.info

mkdir -p coverage_reports
genhtml AllTests_Filtered.info -o coverage_reports

if [ "$1" == "--show-results" ]; then
  open coverage_reports/index.html
fi

popd
popd
