#!/bin/bash -ex

# Copyright 2020-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

# This is the script for the official Couchbase build server.  Do not try to use it, it will only confuse you.
# You have been warned.

set -e

NDK_VER="27.2.12479018"
CMAKE_VER="3.28.1"
NINJA_VER="1.10.2"
PKG_TYPE="zip"
PKG_CMD="zip -r"

function usage() {
    echo "Usage: $0 <source path> <sdk path> <version> <build num> <arch> <edition> <sha_version>"
    exit 1
}

if [ "$#" -ne 7 ]; then
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

if [[ "$EDITION" == "enterprise" ]]; then
    echo "Building enterprise edition (EDITION = enterprise)"
    build_enterprise="ON"
else
    echo "Building community edition (EDITION = $EDITION)"
    build_enterprise="OFF"
fi

SHA_VERSION="$7"
if [ -z "$SHA_VERSION" ]; then
    usage
fi

SDK_MGR="${SDK_HOME}/cmdline-tools/latest/bin/sdkmanager"
CMAKE_PATH="${SDK_HOME}/cmake/${CMAKE_VER}/bin"

BUILD_REL_TARGET="build_${ANDROID_ARCH}_release"
BUILD_DEBUG_TARGET="build_${ANDROID_ARCH}_debug"
PROP_FILE="${SOURCE_PATH}/publish_${ANDROID_ARCH}.prop"
mkdir -p ${SOURCE_PATH}/${BUILD_REL_TARGET} ${SOURCE_PATH}/${BUILD_DEBUG_TARGET}

echo " ======== Installing Toolchain with NDK ${NDK_VER} (this will accept the licenses!)"
yes | ${SDK_MGR} --licenses > /dev/null 2>&1
${SDK_MGR} --install "ndk;${NDK_VER}"

echo " ======== Installing cbdeps ========"
mkdir -p .tools
if [ ! -f .tools/cbdep ]; then 
    curl -o .tools/cbdep http://downloads.build.couchbase.com/cbdep/cbdep.$(uname -s | tr "[:upper:]" "[:lower:]")-$(uname -m)
    chmod +x .tools/cbdep
fi 

CMAKE="$(pwd)/.tools/cmake-${CMAKE_VER}/bin/cmake"
NINJA="$(pwd)/.tools/ninja-${NINJA_VER}/bin/ninja"
if [ ! -f ${CMAKE} ]; then
    .tools/cbdep install -d .tools cmake ${CMAKE_VER}
fi

if [ ! -f ${NINJA} ]; then
    .tools/cbdep install -d .tools ninja ${NINJA_VER}
fi

ARCH_VERSION="24"

#create artifacts dir for publishing to latestbuild
ARTIFACTS_SHA_DIR=${WORKSPACE}/artifacts/couchbase-lite-core/sha/${SHA_VERSION:0:2}/${SHA_VERSION}
ARTIFACTS_BUILD_DIR=${WORKSPACE}/artifacts/couchbase-lite-core/${VERSION}/${BLD_NUM}
mkdir -p ${ARTIFACTS_SHA_DIR}
mkdir -p ${ARTIFACTS_BUILD_DIR}

function create_monolithic_static_lib() {
    tmp_mri=$(mktemp)
    { 
        echo "create libLiteCoreFull.a"
        echo "addlib libLiteCoreStatic.a"
        if [[ "$EDITION" == "enterprise" ]]; then
            echo "addlib EE_P2P/libmDNSResponder.a"
            echo "addlib libLiteCoreWebSocket.a"
        fi
        echo "addlib libCouchbaseSqlite3.a"
        echo "addlib Networking/BLIP/vendor/zlib/libz.a"
        echo "addlib vendor/sqlite3-unicodesn/libSQLite3_UnicodeSN.a"
        echo "addlib vendor/mbedtls/library/libmbedx509.a"
        echo "addlib vendor/mbedtls/library/libmbedcrypto.a"
        echo "addlib vendor/mbedtls/library/libmbedtls.a"
        echo "save"
        echo "end"
    } > "${tmp_mri}"
    ar -M < "${tmp_mri}"
    rm -f "${tmp_mri}"
    ranlib libLiteCoreFull.a
    cp libLiteCoreFull.a install/lib/
}

echo "====  Building Android $ARCH_VERSION Release binary  ==="
cd "${SOURCE_PATH}/${BUILD_REL_TARGET}"
${CMAKE} \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${SDK_HOME}/ndk/${NDK_VER}/build/cmake/android.toolchain.cmake" \
    -DCMAKE_MAKE_PROGRAM="${NINJA}" \
    -DANDROID_PLATFORM=${ARCH_VERSION} \
    -DANDROID_ABI=${ANDROID_ARCH} \
    -DBUILD_ENTERPRISE=${build_enterprise} \
    -DEMBEDDED_MDNS=ON \
    -DCMAKE_INSTALL_PREFIX=`pwd`/install \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DVERSION=${VERSION} \
    -DBLD_NUM=${BLD_NUM} \
    -S ../couchbase-lite-core

${NINJA} install
create_monolithic_static_lib


echo "====  Building Android $ARCH_VERSION Debug binary  ==="
cd ${SOURCE_PATH}/${BUILD_DEBUG_TARGET}
${CMAKE} \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${SDK_HOME}/ndk/${NDK_VER}/build/cmake/android.toolchain.cmake" \
    -DCMAKE_MAKE_PROGRAM="${NINJA}" \
    -DANDROID_PLATFORM=${ARCH_VERSION} \
    -DANDROID_ABI=${ANDROID_ARCH} \
    -DBUILD_ENTERPRISE=${build_enterprise} \
    -DEMBEDDED_MDNS=ON \
    -DCMAKE_INSTALL_PREFIX=`pwd`/install \
    -DCMAKE_BUILD_TYPE=Debug \
    -DVERSION=${VERSION} \
    -DBLD_NUM=${BLD_NUM} \
    -S ../couchbase-lite-core

${NINJA} install
create_monolithic_static_lib

# Create zip package
for FLAVOR in release debug;
do
    PACKAGE_NAME="couchbase-lite-core-android-${ANDROID_ARCH}-${SHA_VERSION}-${FLAVOR}.${PKG_TYPE}"
    echo
    echo  "=== Creating ${SOURCE_PATH}/${PACKAGE_NAME} package ==="
    echo

    if [[ ${FLAVOR} == 'debug' ]]
    then
        cd ${SOURCE_PATH}/${BUILD_DEBUG_TARGET}/install
        ${PKG_CMD} ${SOURCE_PATH}/${PACKAGE_NAME} *
        DEBUG_PKG_NAME=${PACKAGE_NAME}
        cp ${SOURCE_PATH}/${PACKAGE_NAME} ${ARTIFACTS_SHA_DIR}/couchbase-lite-core-android-${ANDROID_ARCH}-${FLAVOR}.${PKG_TYPE}
        cp ${SOURCE_PATH}/${PACKAGE_NAME} ${ARTIFACTS_BUILD_DIR}/couchbase-lite-core-${EDITION}-${VERSION}-${BLD_NUM}-android-${ANDROID_ARCH}-${FLAVOR}.${PKG_TYPE}
        cd ${SOURCE_PATH}
    else
        cd ${SOURCE_PATH}/${BUILD_REL_TARGET}/install
        ${PKG_CMD} ${SOURCE_PATH}/${PACKAGE_NAME} *
        RELEASE_PKG_NAME=${PACKAGE_NAME}
        cp ${SOURCE_PATH}/${PACKAGE_NAME} ${ARTIFACTS_SHA_DIR}/couchbase-lite-core-android-${ANDROID_ARCH}.${PKG_TYPE}
        cp ${SOURCE_PATH}/${PACKAGE_NAME} ${ARTIFACTS_BUILD_DIR}/couchbase-lite-core-${EDITION}-${VERSION}-${BLD_NUM}-android-${ANDROID_ARCH}.${PKG_TYPE}
        cd ${SOURCE_PATH}
    fi
done

# Create Nexus publishing prop file
cd ${SOURCE_PATH}
echo "PRODUCT=couchbase-lite-core"  >> ${PROP_FILE}
echo "BLD_NUM=${BLD_NUM}"  >> ${PROP_FILE}
echo "VERSION=${SHA_VERSION}" >> ${PROP_FILE}
echo "PKG_TYPE=${PKG_TYPE}" >> ${PROP_FILE}
echo "DEBUG_PKG_NAME=${DEBUG_PKG_NAME}" >> ${PROP_FILE}
echo "RELEASE_PKG_NAME=${RELEASE_PKG_NAME}" >> ${PROP_FILE}

echo
echo  "=== Created ${PROP_FILE} ==="
echo

cat ${PROP_FILE}
