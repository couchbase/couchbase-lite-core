#!/bin/bash

# Copyright 2020-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

set -e
shopt -s extglob dotglob

NDK_VER="27.0.12077973"
CMAKE_VER="3.28.1"
NINJA_VER="1.10.2"

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

SDK_MGR="${SDK_HOME}/cmdline-tools/latest/bin/sdkmanager"
yes | ${SDK_MGR} --licenses > /dev/null 2>&1
${SDK_MGR} --install "ndk;${NDK_VER}"

mkdir -p .tools
if [ ! -f .tools/cbdep ]; then 
    curl -o .tools/cbdep http://downloads.build.couchbase.com/cbdep/cbdep.$(uname -s | tr "[:upper:]" "[:lower:]")-$(uname -m)
    chmod +x .tools/cbdep
fi 

CMAKE="$(pwd)/.tools/cmake-${CMAKE_VER}/bin/cmake"
NINJA="$(pwd)/.tools/ninja-${NINJA_VER}/bin/ninja"
if [ ! -f ${CMAKE} ]; then
    .tools/cbdep install -d .tools cmake ${CMAKE_VER}
fi

if [ ! -f ${NINJA} ]; then
    .tools/cbdep install -d .tools ninja ${NINJA_VER}
fi

mkdir "couchbase-lite-core"
git submodule update --init --recursive
mv !(couchbase-lite-core|.tools|.|..) couchbase-lite-core

# Sometimes a PR depends on a PR in the EE repo as well.  This needs to be convention based, so if there is a branch with the name PR-###
# (with the GH PR number) in the EE repo then use that, otherwise use the name of the target branch (master, release/XXX etc)
git clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $BRANCH_NAME --recursive --depth 1 couchbase-lite-core-EE || \
    git clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $CHANGE_TARGET --recursive --depth 1 couchbase-lite-core-EE 


# Just build one of each 32 and 64-bit for validation
mkdir -p "couchbase-lite-core/build_cmake/android/armeabi-v7a"
pushd "couchbase-lite-core/build_cmake/android/armeabi-v7a"
${CMAKE} \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${SDK_HOME}/ndk/${NDK_VER}/build/cmake/android.toolchain.cmake" \
    -DANDROID_PLATFORM=22 \
    -DANDROID_ABI=armeabi-v7a \
    -DBUILD_ENTERPRISE=ON \
    -DEMBEDDED_MDNS=ON \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    ../../..

${NINJA} LiteCore
popd

mkdir -p "couchbase-lite-core/build_cmake/android/arm64-v8a"
pushd "couchbase-lite-core/build_cmake/android/arm64-v8a"
${CMAKE} \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${SDK_HOME}/ndk/${NDK_VER}/build/cmake/android.toolchain.cmake" \
    -DANDROID_PLATFORM=22 \
    -DANDROID_ABI=arm64-v8a \
    -DBUILD_ENTERPRISE=ON \
    -DEMBEDDED_MDNS=ON \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    ../../..

${NINJA} LiteCore
popd
