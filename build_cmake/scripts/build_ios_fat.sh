#!/bin/bash

SCRIPT_DIR=`dirname $0`

pushd $SCRIPT_DIR/..
mkdir ios-fat

xcodebuild -project ../Xcode/LiteCore.xcodeproj -configuration Release -derivedDataPath ios -scheme "LiteCore dylib" -sdk iphoneos
xcodebuild -project ../Xcode/LiteCore.xcodeproj -configuration Release -derivedDataPath ios -scheme "LiteCore dylib" -sdk iphonesimulator
lipo -create ios/Build/Products/Release-iphoneos/libLiteCore.dylib ios/Build/Products/Release-iphonesimulator/libLiteCore.dylib -output ios-fat/libLiteCore.dylib
rm -rf ios
popd
