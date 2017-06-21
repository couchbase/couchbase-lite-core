#!/bin/bash

SCRIPT_DIR=`dirname $0`

pushd $SCRIPT_DIR/..
mkdir ios-fat

xcodebuild -project ../Xcode/LiteCore.xcodeproj -configuration Release -derivedDataPath ios -scheme "LiteCore dylib" -sdk iphoneos BITCODE_GENERATION_MODE=bitcode "CODE_SIGNING_REQUIRED=NO" "CODE_SIGN_IDENTITY="
xcodebuild -project ../Xcode/LiteCore.xcodeproj -configuration Release -derivedDataPath ios -scheme "LiteCore dylib" -sdk iphonesimulator "CODE_SIGNING_REQUIRED=NO" "CODE_SIGN_IDENTITY="
lipo -create ios/Build/Products/Release-iphoneos/libLiteCore.dylib ios/Build/Products/Release-iphonesimulator/libLiteCore.dylib -output ios-fat/libLiteCore.dylib
rm -rf ios
popd
