#!/bin/bash

set -e

OUTPUT_DIR="`pwd`/../prebuilt"

rm -f $OUTPUT_DIR/libCBForest-Interop.dylib
rm -rf $OUTPUT_DIR/libCBForest-Interop.dylib.dSYM

pushd ../../
rm -rf build
xcodebuild -scheme "CBForest-Interop" -configuration Release -derivedDataPath build clean build

mv build/Build/Products/Release/libCBForest-Interop.dylib $OUTPUT_DIR/libCBForest-Interop.dylib
mv build/Build/Products/Release/libCBForest-Interop.dylib.dSYM $OUTPUT_DIR/libCBForest-Interop.dylib.dSYM

rm -rf build
popd
