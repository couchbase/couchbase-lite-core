#!/bin/bash

set -e

OUTPUT_DIR="`pwd`/../prebuilt"

rm -f $OUTPUT_DIR/libCBForest-Interop.dylib
pushd ../../
rm -rf build
xcodebuild ARCHS="i386 x86_64" -scheme "CBForest-Interop" -configuration Release -derivedDataPath build clean build

mv build/Build/Products/Release/libCBForest-Interop.dylib $OUTPUT_DIR/libCBForest-Interop.dylib

rm -rf build
popd
