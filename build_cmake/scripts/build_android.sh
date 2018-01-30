#!/bin/bash

SCRIPT_DIR=`dirname $0`
pushd $SCRIPT_DIR/..

core_count=`getconf _NPROCESSORS_ONLN`
for arch in x86 armeabi-v7a arm64-v8a; do
  mkdir -p android/lib/$arch
  cp scripts/strip.sh android/lib/$arch
  pushd android/lib/$arch
  version=16
  if [ "$arch" = "arm64-v8a" ]; then
    version=21
  fi
  cmake -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_SYSTEM_NAME=Android -DCMAKE_SYSTEM_VERSION=$version -DCMAKE_ANDROID_NDK_TOOLCHAIN_VERSION=clang -DCMAKE_ANDROID_ARCH_ABI=$arch -DCMAKE_ANDROID_STL_TYPE=c++_static ../../../..
  make -j `expr $core_count + 1` LiteCore
  if [ "$arch" = "arm64-v8a" ]; then
    STRIP=`dirname $(find $ANDROID_NDK_ROOT/toolchains -name strip | grep aarch64)`
  elif [ "$arch" = "armeabi-v7a" ]; then
    STRIP=`dirname $(find $ANDROID_NDK_ROOT/toolchains -name strip | grep arm)`
  else
    STRIP=`dirname $(find $ANDROID_NDK_ROOT/toolchains -name strip | grep x86-)`
  fi

  if [-d "couchbase-lite-core"]; then
    ./strip.sh `pwd`/couchbase-lite-core $STRIP/
  else
    ./strip.sh `pwd` $STRIP/
  fi

  popd
done

popd
