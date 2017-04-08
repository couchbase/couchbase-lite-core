#!/bin/bash

SCRIPT_DIR=`dirname $0`
ENABLE_BITCODE=""
if [ $1 ]; then
  if [[ "$1" == "--enable-bitcode" ]]; then
    ENABLE_BITCODE="-DCMAKE_ENABLE_BITCODE=Yes"
  fi
fi

pushd $SCRIPT_DIR/..
mkdir tvos
mkdir tvos-sim
mkdir tvos-fat

pushd tvos
cmake -DBLIP_NO_FRAMING=ON -DCMAKE_TOOLCHAIN_FILE=../scripts/AppleDevice.cmake -DCMAKE_PLATFORM=TVOS -DIOS_DEPLOYMENT_TARGET=9.0 -DCMAKE_BUILD_TYPE=RelWithDebInfo $ENABLE_BITCODE ../..
make -j8 LiteCore

popd
pushd tvos-sim
cmake -DBLIP_NO_FRAMING=ON -DCMAKE_TOOLCHAIN_FILE=../scripts/AppleDevice.cmake -DCMAKE_PLATFORM=TVOS-SIMULATOR -DIOS_DEPLOYMENT_TARGET=9.0 -DCMAKE_BUILD_TYPE=RelWithDebInfo ../..
make -j8 LiteCore

popd
lipo -create tvos/libLiteCore.dylib tvos-sim/libLiteCore.dylib -output tvos-fat/libLiteCore.dylib
rm -rf tvos
rm -rf tvos-sim

popd
