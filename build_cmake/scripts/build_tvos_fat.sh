#!/bin/bash

SCRIPT_DIR=`dirname $0`

pushd $SCRIPT_DIR/..
mkdir tvos-fat

xcodebuild -project ../Xcode/LiteCore.xcodeproj -configuration Release -derivedDataPath tvos -scheme "LiteCore dylib" -sdk appletvos BITCODE_GENERATION_MODE=bitcode CODE_SIGNING_ALLOWED=NO
xcodebuild -project ../Xcode/LiteCore.xcodeproj -configuration Release -derivedDataPath tvos -scheme "LiteCore dylib" -sdk appletvsimulator CODE_SIGNING_ALLOWED=NO
lipo -create tvos/Build/Products/Release-appletvos/libLiteCore.dylib tvos/Build/Products/Release-appletvsimulator/libLiteCore.dylib -output tvos-fat/libLiteCore.dylib

rm -rf tvos

popd
