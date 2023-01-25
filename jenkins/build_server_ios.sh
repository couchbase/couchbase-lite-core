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

BIN_NAME="LiteCore"
FRAMEWORK_LOC=${BIN_NAME}.xcarchive/Products/Library/Frameworks/${BIN_NAME}.framework

# this will be used to collect all destination framework path with `-framework`
# to include them in `-create-xcframework`
FRAMEWORK_PATH_ARGS=()

# arg1 = target destination for which the archive is built for. E.g., "generic/platform=iOS"
# arg2 = Configuration for building (Release, Debug_EE, etc)
function xcarchive
{
  DESTINATION=${1}
  CONFIGURATION=${2}
  echo "Archiving for ${DESTINATION}..."
  ARCHIVE_PATH=$PWD/$(echo ${DESTINATION} | sed 's/ /_/g' | sed 's/\//\_/g')
  xcodebuild archive \
    -project "$WORKSPACE/$ios_xcode_proj" \
    -scheme "LiteCore framework" \
    -configuration "${CONFIGURATION}" \
    -destination "${DESTINATION}" \
    -archivePath "${ARCHIVE_PATH}/${BIN_NAME}.xcarchive" \
    "ONLY_ACTIVE_ARCH=NO" "BITCODE_GENERATION_MODE=bitcode" \
    "CODE_SIGNING_ALLOWED=NO" \
    "SKIP_INSTALL=NO"
  
  if [ -f "${ARCHIVE_PATH}/${BIN_NAME}.xcarchive/dSYMs/${BIN_NAME}.framework.dSYM" ]; then
    FRAMEWORK_PATH_ARGS+=("-framework "${ARCHIVE_PATH}/${FRAMEWORK_LOC}" \
        -debug-symbols "${ARCHIVE_PATH}/${BIN_NAME}.xcarchive/dSYMs/${BIN_NAME}.framework.dSYM"")
  else
    # Not present when making debug build for some reason (included inline?)
    FRAMEWORK_PATH_ARGS+=("-framework "${ARCHIVE_PATH}/${FRAMEWORK_LOC})
  fi
  echo "Finished archiving ${DESTINATION}."
}

echo "====  Building ios Release binary  ==="
cd ${WORKSPACE}/${BUILD_IOS_REL_TARGET}
xcarchive "generic/platform=iOS Simulator" ${release_config}
xcarchive "generic/platform=iOS" ${release_config}
xcarchive "generic/platform=macOS,variant=Mac Catalyst" ${release_config}
xcodebuild -create-xcframework -output "${WORKSPACE}/${BUILD_IOS_REL_TARGET}/${BIN_NAME}.xcframework" ${FRAMEWORK_PATH_ARGS[*]}
cd ${WORKSPACE}

FRAMEWORK_PATH_ARGS=()
echo "====  Building ios Debug binary  ==="
cd ${WORKSPACE}/${BUILD_IOS_DEBUG_TARGET}
xcarchive "generic/platform=iOS Simulator" ${debug_config}
xcarchive "generic/platform=iOS" ${debug_config}
xcarchive "generic/platform=macOS,variant=Mac Catalyst" ${debug_config}
xcodebuild -create-xcframework -output "${WORKSPACE}/${BUILD_IOS_DEBUG_TARGET}/${BIN_NAME}.xcframework" ${FRAMEWORK_PATH_ARGS[*]}
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
        ${PKG_CMD} ${WORKSPACE}/${PACKAGE_NAME} LiteCore.xcframework
        cd ${WORKSPACE}
        DEBUG_IOS_PKG_NAME=${PACKAGE_NAME}
    else
        cd ${WORKSPACE}/${BUILD_IOS_REL_TARGET}
        ${PKG_CMD} ${WORKSPACE}/${PACKAGE_NAME} LiteCore.xcframework
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
