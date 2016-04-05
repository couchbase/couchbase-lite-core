#!/bin/bash

set -e

OUTPUT_DIR="`pwd`/../prebuilt"

rm -f $OUTPUT_DIR/libCBForest-Interop.a

pushd ../../
rm -rf build
xcodebuild -scheme "CBForest static" -configuration Release -derivedDataPath build -sdk iphoneos
xcodebuild -scheme "CBForest static" -configuration Release -derivedDataPath build -sdk iphonesimulator -destination 'platform=iOS Simulator,name=iPhone 6,OS=latest'

lipo -create -output $OUTPUT_DIR/libCBForest-Interop.a build/Build/Products/Release-iphoneos/libCBForest.a build/Build/Products/Release-iphonesimulator/libCBForest.a

rm -rf build
popd
