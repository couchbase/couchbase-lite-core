#!/bin/bash -ex

# This is the script for the official Couchbase build server.  Do not try to use it, it will only confuse you.
# You have been warned.

set -e

CMAKE_VER="3.10.2.4988404"
NDK_VER="21.2.6472646"
PKG_TYPE="zip"
PKG_CMD="zip -r"

function usage() {
    echo "Usage: $0 <source path> <sdk path> <version> <build num> <arch> <edition>"
    exit 1
}

if [ "$#" -ne 6 ]; then
    usage
fi

SOURCE_PATH="$1"
if [ -z "$SOURCE_PATH" ]; then
    usage
fi

SDK_HOME="$2"
if [ -z "$SDK_HOME" ]; then
    usage
fi

VERSION="$3"
if [ -z "$VERSION" ]; then
    usage
fi

BLD_NUM="$4"
if [ -z "$BLD_NUM" ]; then
    usage
fi

ANDROID_ARCH="$5"
if [ -z "$ANDROID_ARCH" ]; then
    usage
fi

EDITION="$6"
if [ -z "$EDITION" ]; then
    usage
fi

SDK_MGR="${SDK_HOME}/tools/bin/sdkmanager"
CMAKE_PATH="${SDK_HOME}/cmake/${CMAKE_VER}/bin"

BUILD_REL_TARGET="build_${ANDROID_ARCH}_release"
BUILD_DEBUG_TARGET="build_${ANDROID_ARCH}_debug"
PROP_FILE="${SOURCE_PATH}/publish_${ANDROID_ARCH}.prop"
mkdir -p ${SOURCE_PATH}/${BUILD_REL_TARGET} ${SOURCE_PATH}/${BUILD_DEBUG_TARGET}

echo " ======== Installing Toolchain with CMake ${CMAKE_VER} and NDK ${NDK_VER} (this will accept the licenses!)"
yes | ${SDK_MGR} --licenses > /dev/null 2>&1
${SDK_MGR} --install "cmake;${CMAKE_VER}"
${SDK_MGR} --install "ndk;${NDK_VER}"

ARCH_VERSION="19"
if [[ "${ANDROID_ARCH}" == "x86_64" ]] || [[ "${ANDROID_ARCH}" == "arm64-v8a" ]]; then
    ARCH_VERSION="21"
fi

echo "====  Building Android $ARCH_VERSION Release binary  ==="
cd "${SOURCE_PATH}/${BUILD_REL_TARGET}"
${CMAKE_PATH}/cmake \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${SDK_HOME}/ndk/${NDK_VER}/build/cmake/android.toolchain.cmake" \
    -DCMAKE_MAKE_PROGRAM="${CMAKE_PATH}/ninja" \
    -DANDROID_NATIVE_API_LEVEL=${ARCH_VERSION} \
    -DANDROID_ABI=${ANDROID_ARCH} \
    -DEDITION=${EDITION} \
    -DCMAKE_INSTALL_PREFIX=`pwd`/install \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    ..

${CMAKE_PATH}/ninja install/strip

echo "====  Building Android $ARCH_VERSION Debug binary  ==="
cd ${SOURCE_PATH}/${BUILD_DEBUG_TARGET}
${CMAKE_PATH}/cmake \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${SDK_HOME}/ndk/${NDK_VER}/build/cmake/android.toolchain.cmake" \
    -DCMAKE_MAKE_PROGRAM="${CMAKE_PATH}/ninja" \
    -DANDROID_NATIVE_API_LEVEL=${ARCH_VERSION} \
    -DANDROID_ABI=${ANDROID_ARCH} \
    -DEDITION=${EDITION} \
    -DCMAKE_INSTALL_PREFIX=`pwd`/install \
    -DCMAKE_BUILD_TYPE=Debug \
    ..

${CMAKE_PATH}/ninja install/strip

# Create zip package
for FLAVOR in release debug;
do
    PACKAGE_NAME="couchbase-lite-core-android-${ANDROID_ARCH}-${VERSION}-${FLAVOR}.${PKG_TYPE}"
    echo
    echo  "=== Creating ${SOURCE_PATH}/${PACKAGE_NAME} package ==="
    echo

    if [[ ${FLAVOR} == 'debug' ]]
    then
        cd ${SOURCE_PATH}/${BUILD_DEBUG_TARGET}/install
        ${PKG_CMD} ${SOURCE_PATH}/${PACKAGE_NAME} *
        DEBUG_PKG_NAME=${PACKAGE_NAME}
        cd ${SOURCE_PATH}
    else
        cd ${SOURCE_PATH}/${BUILD_REL_TARGET}/install
        ${PKG_CMD} ${SOURCE_PATH}/${PACKAGE_NAME} *
        RELEASE_PKG_NAME=${PACKAGE_NAME}
        cd ${SOURCE_PATH}
    fi
done

# Create Nexus publishing prop file
cd ${SOURCE_PATH}
echo "PRODUCT=couchbase-lite-core"  >> ${PROP_FILE}
echo "BLD_NUM=${BLD_NUM}"  >> ${PROP_FILE}
echo "VERSION=${VERSION}" >> ${PROP_FILE}
echo "PKG_TYPE=${PKG_TYPE}" >> ${PROP_FILE}
echo "DEBUG_PKG_NAME=${DEBUG_PKG_NAME}" >> ${PROP_FILE}
echo "RELEASE_PKG_NAME=${RELEASE_PKG_NAME}" >> ${PROP_FILE}

echo
echo  "=== Created ${PROP_FILE} ==="
echo

cat ${PROP_FILE}
