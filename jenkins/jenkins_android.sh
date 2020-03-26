#!/bin/bash

set -e
shopt -s extglob dotglob

CMAKE_VER="3.10.2.4988404"
NDK_VER="20.1.5948944"

function usage() {
    echo "Usage: $0 <sdk path>"
    exit 1
}

if ! [ -x "$(command -v git)" ]; then
  echo 'Error: git is not installed.' >&2
  exit 1
fi

SDK_HOME="$1"
if [ -z "$SDK_HOME" ]; then
    usage
fi

SDK_MGR="${SDK_HOME}/tools/bin/sdkmanager"
CMAKE_PATH="${SDK_HOME}/cmake/${CMAKE_VER}/bin"
yes | ${SDK_MGR} --licenses > /dev/null 2>&1
${SDK_MGR} --install "cmake;${CMAKE_VER}"
${SDK_MGR} --install "ndk;${NDK_VER}"

mkdir "couchbase-lite-core"
git submodule update --init --recursive
mv !(couchbase-lite-core) couchbase-lite-core

# Sometimes a PR depends on a PR in the EE repo as well.  This needs to be convention based, so if there is a branch with the name PR-###
# (with the GH PR number) in the EE repo then use that, otherwise use the name of the target branch (master, release/XXX etc)
git clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $BRANCH_NAME --recursive --depth 1 couchbase-lite-core-EE || \
    git clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $CHANGE_TARGET --recursive --depth 1 couchbase-lite-core-EE 

# Just build one of each 32 and 64-bit for validation
mkdir -p "couchbase-lite-core/build_cmake/android/armeabi-v7a"
pushd "couchbase-lite-core/build_cmake/android/armeabi-v7a"
${CMAKE_PATH}/cmake \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${SDK_HOME}/ndk/${NDK_VER}/build/cmake/android.toolchain.cmake" \
    -DCMAKE_MAKE_PROGRAM="${CMAKE_PATH}/ninja" \
    -DANDROID_NATIVE_API_LEVEL=19 \
    -DANDROID_ABI=armeabi-v7a \
    -DBUILD_ENTERPRISE=ON \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    ../../..

${CMAKE_PATH}/ninja
popd

mkdir -p "couchbase-lite-core/build_cmake/android/arm64-v8a"
pushd "couchbase-lite-core/build_cmake/android/arm64-v8a"
${CMAKE_PATH}/cmake \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${SDK_HOME}/ndk/${NDK_VER}/build/cmake/android.toolchain.cmake" \
    -DCMAKE_MAKE_PROGRAM="${CMAKE_PATH}/ninja" \
    -DANDROID_NATIVE_API_LEVEL=21 \
    -DANDROID_ABI=arm64-v8a \
    -DBUILD_ENTERPRISE=ON \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    ../../..

${CMAKE_PATH}/ninja
popd