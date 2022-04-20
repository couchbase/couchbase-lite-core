#! /bin/bash -e
# This script builds the mbedTLS submodule.
# It's run by Xcode when building the target LiteCoreWebSocket.

source "$SRCROOT/build_setup.sh" mbedtls

# Set up the CMake build options:
CMAKE_OPTS="$CMAKE_OPTS \
            -DENABLE_PROGRAMS=0 \
            -DENABLE_TESTING=0"

if [[ "$CONFIGURATION" == "Release*" ]]
then
    CMAKE_OPTS="$CMAKE_OPTS -DCMAKE_BUILD_TYPE=RelWithDebInfo"
else
    CMAKE_OPTS="$CMAKE_OPTS -DCMAKE_BUILD_TYPE=Debug"
    if [ "$ENABLE_ADDRESS_SANITIZER" == "YES" ]
    then
        CMAKE_OPTS="$CMAKE_OPTS -DLWS_WITH_ASAN=1"
    fi
fi

echo "CMake options: $CMAKE_OPTS"

# Build!
if [[ "$EFFECTIVE_PLATFORM_NAME" == "-maccatalyst" ]]
then
    cmake "$SRCROOT/../vendor/mbedtls" $CMAKE_OPTS \
        '-DCMAKE_CXX_FLAGS=-target x86_64-apple-ios13.1-macabi' \
        '-DCMAKE_C_FLAGS=-target x86_64-apple-ios13.1-macabi'
else
    cmake "$SRCROOT/../vendor/mbedtls" $CMAKE_OPTS
fi
make

# Copy the resulting static libraries to the Xcode build dir where the linker will find them:
mkdir -p "$BUILT_PRODUCTS_DIR"
cp -pv library/libmbed*.a "$BUILT_PRODUCTS_DIR/"
