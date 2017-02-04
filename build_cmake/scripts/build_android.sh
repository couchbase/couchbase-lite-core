#!/bin/bash

SCRIPT_DIR=`dirname $0`
pushd $SCRIPT_DIR/..

core_count=`getconf _NPROCESSORS_ONLN`
for arch in x86 armeabi-v7a arm64-v8a; do
  mkdir -p android/lib/$arch
  pushd android/lib/$arch
  version=16
  if [ "$arch" = "arm64-v8a" ]; then
    version=21
  fi
  cmake -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_SYSTEM_NAME=Android -DCMAKE_SYSTEM_VERSION=$version -DCMAKE_ANDROID_NDK_TOOLCHAIN_VERSION=clang -DCMAKE_ANDROID_ARCH_ABI=$arch -DCMAKE_ANDROID_STL_TYPE=c++_static ../../../..
  make -j `expr $core_count + 1` LiteCore
  popd
done

popd
