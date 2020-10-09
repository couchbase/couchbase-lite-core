#!/bin/bash -ex

script_dir=`dirname $0`
cmake_dir=$script_dir/../build_cmake/valgrind
mkdir -p $cmake_dir

pushd $cmake_dir
cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_ENTERPRISE=ON ../..
make -j8 C4Tests
popd

valgrind --leak-check=yes $cmake_dir/C/tests/C4Tests -r list 2>&1 | tee memcheck.txt
valgrind --tool=drd $cmake_dir/C/tests/C4Tests -r list 2>&1 | tee drd.txt