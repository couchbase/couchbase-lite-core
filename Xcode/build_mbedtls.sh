#! /bin/bash -e
# This script builds the mbedTLS submodule.
# It's run by Xcode when building the target LiteCoreWebSocket.

# Create the CMake build directory:
CMAKE_BUILD_DIR="$PER_VARIANT_OBJECT_FILE_DIR/mbedtls"
mkdir -p "$CMAKE_BUILD_DIR"
cd "$CMAKE_BUILD_DIR"

# Set up the CMake build options:
CMAKE_OPTS="-DCMAKE_BUILD_TYPE=RelWithDebugInfo -DENABLE_PROGRAMS=0 -DENABLE_TESTING=0"

# Build!
cmake "$SRCROOT/../vendor/mbedtls" $CMAKE_OPTS
make

# Copy the resulting static libraries to the Xcode build dir where the linker will find them:
mkdir -p "$TARGET_BUILD_DIR"
cp library/libmbed*.a "$TARGET_BUILD_DIR/"
