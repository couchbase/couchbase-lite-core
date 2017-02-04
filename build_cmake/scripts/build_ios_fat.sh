#!/bin/bash

SCRIPT_DIR=`dirname $0`
ENABLE_BITCODE=""
if [ $1 ]; then
  if [[ "$1" == "--enable-bitcode" ]]; then
    ENABLE_BITCODE="-DCMAKE_ENABLE_BITCODE=Yes"
  fi
fi

pushd $SCRIPT_DIR/..
mkdir ios
mkdir ios-sim
mkdir ios-fat

pushd ios
cmake -DCMAKE_TOOLCHAIN_FILE=../scripts/AppleDevice.cmake -DCMAKE_PLATFORM=IOS -DIOS_DEPLOYMENT_TARGET=8.0 -DCMAKE_BUILD_TYPE=RelWithDebInfo $ENABLE_BITCODE ../..
make -j8 LiteCore

popd
pushd ios-sim
cmake -DCMAKE_TOOLCHAIN_FILE=../scripts/AppleDevice.cmake -DCMAKE_PLATFORM=IOS-SIMULATOR -DIOS_DEPLOYMENT_TARGET=8.0 -DCMAKE_BUILD_TYPE=RelWithDebInfo ../..
make -j8 LiteCore

popd
lipo -create ios/libLiteCore.dylib ios-sim/libLiteCore.dylib -output ios-fat/libLiteCore.dylib
rm -rf ios
rm -rf ios-sim

popd
