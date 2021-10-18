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

# Global define
PRODUCT=${1}
BLD_NUM=${2}
VERSION=${3}
EDITION=${4}

if [[ -z "${WORKSPACE}" ]]; then
    WORKSPACE=`pwd`
fi

PKG_CMD='zip -r'
PKG_TYPE='zip'
OS="macosx-ios"
BUILD_IOS_REL_TARGET='build_ios_release'
BUILD_IOS_DEBUG_TARGET='build_ios_debug'
PROP_FILE=${WORKSPACE}/publish_ios.prop

mkdir -p ${WORKSPACE}/${BUILD_IOS_REL_TARGET} ${WORKSPACE}/${BUILD_IOS_DEBUG_TARGET}
project_dir=couchbase-lite-core
strip_dir=${project_dir}
ios_xcode_proj="couchbase-lite-core/Xcode/LiteCore.xcodeproj"

if [[ ${EDITION} == 'enterprise' ]]; then
    release_config="Release_EE"
    debug_config="Debug_EE"
else
    release_config="Release"
    debug_config="Debug"
fi

echo VERSION=${VERSION}
# Global define end

echo "====  Building ios Release binary  ==="
cd ${WORKSPACE}/${BUILD_IOS_REL_TARGET}
xcodebuild -project "${WORKSPACE}/${ios_xcode_proj}" -configuration ${release_config} -derivedDataPath ios -scheme "LiteCore framework" -sdk iphoneos BITCODE_GENERATION_MODE=bitcode CODE_SIGNING_ALLOWED=NO
xcodebuild -project "${WORKSPACE}/${ios_xcode_proj}" -configuration ${release_config} -derivedDataPath ios -scheme "LiteCore framework" -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO
cp -R ios/Build/Products/${release_config}-iphoneos/LiteCore.framework ${WORKSPACE}/${BUILD_IOS_REL_TARGET}/
lipo -create ios/Build/Products/${release_config}-iphoneos/LiteCore.framework/LiteCore ios/Build/Products/${release_config}-iphonesimulator/LiteCore.framework/LiteCore -output ${WORKSPACE}/${BUILD_IOS_REL_TARGET}/LiteCore.framework/LiteCore
cd ${WORKSPACE}

echo "====  Building ios Debug binary  ==="
cd ${WORKSPACE}/${BUILD_IOS_DEBUG_TARGET}
xcodebuild -project "${WORKSPACE}/${ios_xcode_proj}" -configuration ${debug_config} -derivedDataPath ios -scheme "LiteCore framework" -sdk iphoneos BITCODE_GENERATION_MODE=bitcode CODE_SIGNING_ALLOWED=NO
xcodebuild -project "${WORKSPACE}/${ios_xcode_proj}" -configuration ${debug_config} -derivedDataPath ios -scheme "LiteCore framework" -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO
cp -R ios/Build/Products/${debug_config}-iphoneos/LiteCore.framework ${WORKSPACE}/${BUILD_IOS_DEBUG_TARGET}/
lipo -create ios/Build/Products/${debug_config}-iphoneos/LiteCore.framework/LiteCore ios/Build/Products/${debug_config}-iphonesimulator/LiteCore.framework/LiteCore -output ${WORKSPACE}/${BUILD_IOS_DEBUG_TARGET}/LiteCore.framework/LiteCore
cd ${WORKSPACE}

# Create zip package
for FLAVOR in release debug;
do
    PACKAGE_NAME=${PRODUCT}-${OS}-${VERSION}-${FLAVOR}.${PKG_TYPE}
    echo
    echo  "=== Creating ${WORKSPACE}/${PACKAGE_NAME} package ==="
    echo

    if [[ "${FLAVOR}" == 'debug' ]]
    then
        cd ${WORKSPACE}/${BUILD_IOS_DEBUG_TARGET}
        ${PKG_CMD} ${WORKSPACE}/${PACKAGE_NAME} LiteCore.framework
        cd ${WORKSPACE}
        DEBUG_IOS_PKG_NAME=${PACKAGE_NAME}
    else
        cd ${WORKSPACE}/${BUILD_IOS_REL_TARGET}
        ${PKG_CMD} ${WORKSPACE}/${PACKAGE_NAME} LiteCore.framework
        cd ${WORKSPACE}
        RELEASE_IOS_PKG_NAME=${PACKAGE_NAME}
    fi
done

# Create Nexus publishing prop file
cd ${WORKSPACE}
echo "PRODUCT=${PRODUCT}"  >> ${PROP_FILE}
echo "BLD_NUM=${BLD_NUM}"  >> ${PROP_FILE}
echo "VERSION=${VERSION}" >> ${PROP_FILE}
echo "PKG_TYPE=${PKG_TYPE}" >> ${PROP_FILE}
echo "DEBUG_IOS_PKG_NAME=${DEBUG_IOS_PKG_NAME}" >> ${PROP_FILE}
echo "RELEASE_IOS_PKG_NAME=${RELEASE_IOS_PKG_NAME}" >> ${PROP_FILE}
echo
echo  "=== Created ${WORKSPACE}/${PROP_FILE} ==="
echo

cat ${PROP_FILE}
