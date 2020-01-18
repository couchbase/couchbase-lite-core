#! /bin/bash -e
# This script does some setup before using CMake to build a library.
# It's used by build_libwebsockets.sh and build_mbedtls.sh.

# Create the CMake build directory:
if [[ "$PER_VARIANT_OBJECT_FILE_DIR" != "" ]]
then
    CMAKE_BUILD_DIR="$PER_VARIANT_OBJECT_FILE_DIR/$1"
else
    CMAKE_BUILD_DIR="$OBJECT_FILE_DIR/$1"
fi

mkdir -p "$CMAKE_BUILD_DIR"
cd "$CMAKE_BUILD_DIR"

# Define an initial CMAKE_OPTS including compiler options for iOS.
# The main build script should then append to this and pass it to cmake.
if [[ "$PLATFORM_NAME" == "iphoneos" ]]
then
    CMAKE_OPTS="$CMAKE_OPTS \
                -DCMAKE_TOOLCHAIN_FILE=$SRCROOT/../vendor/ios-cmake/ios.toolchain.cmake \
                -DPLATFORM=OS \
                -DDEPLOYMENT_TARGET=$IPHONEOS_DEPLOYMENT_TARGET"
elif [[ "$PLATFORM_NAME" == "iphonesimulator" ]]
then
    CMAKE_OPTS="$CMAKE_OPTS \
                -DCMAKE_TOOLCHAIN_FILE=$SRCROOT/../vendor/ios-cmake/ios.toolchain.cmake \
                -DPLATFORM=SIMULATOR64 \
                -DDEPLOYMENT_TARGET=$IPHONEOS_DEPLOYMENT_TARGET"
fi
