#!/bin/sh

SCRIPT_DIR=`dirname $0`
pushd $SCRIPT_DIR/..

mkdir -p macos
pushd macos
cmake -DCMAKE_BUILD_TYPE=Debug -DCODE_COVERAGE_ENABLED=ON ../..
core_count=`getconf _NPROCESSORS_ONLN`
make -j `expr $core_count + 1`

pushd LiteCore/tests
./CppTests -r list
popd

lcov -d CMakeFiles/LiteCoreStatic.dir/ -d vendor/fleece -d vendor/BLIP-Cpp -c -o CppTests.info
find . -type f -name '*.gcda' -delete

pushd C/tests
./C4Tests -r list
popd

lcov -d CMakeFiles/LiteCoreStatic.dir/ -d vendor/fleece -d vendor/BLIP-Cpp -c -o C4Tests.info
find . -type f -name '*.gcda' -delete

lcov -a CppTests.info -a C4Tests.info -o AllTests.info
lcov --remove AllTests.info '/usr/include/*' '/System/*' '/Applications/*' '*/vendor/SQLiteCpp/*' '*/vendor/BLIP-Cpp/tests/*' '*/vendor/fleece/ObjC/*' '*/vendor/fleece/vendor/*' '*/vendor/BLIP-Cpp/vendor/*' '*/C/c4DocEnumerator.cc' -o AllTests_Filtered.info

mkdir -p coverage_reports
genhtml AllTests_Filtered.info -o coverage_reports

if [ "$1" == "--show-results" ]; then
  open coverage_reports/index.html
fi

popd
popd
