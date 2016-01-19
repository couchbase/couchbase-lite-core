#!/bin/bash

set -e

OUTPUT_DIR="`pwd`/CSharp/prebuilt"

rm -f $OUTPUT_DIR/libCBForest-Interop.a

rm -rf build
xcodebuild -scheme "CBForest-Interop iOS" -configuration Release -derivedDataPath build
xcodebuild -scheme "CBForest-Interop iOS" -configuration Release -derivedDataPath build -sdk iphonesimulator -destination 'platform=iOS Simulator,name=iPhone 6,OS=latest'

lipo -create -output $OUTPUT_DIR/libCBForest-Interop.a build/Build/Products/Release-iphoneos/libCBForest-Interop.a build/Build/Products/Release-iphonesimulator/libCBForest-Interop.a

rm -rf build
