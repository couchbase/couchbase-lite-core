#!/bin/bash -ex

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