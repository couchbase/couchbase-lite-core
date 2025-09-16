#!/bin/bash -ex

# Copyright 2020-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

set -x
# Global define
PRODUCT=${1}
VERSION=${2}
BLD_NUM=${3}
EDITION=${4}
SHA_VERSION=${5}

if [[ -z "${WORKSPACE}" ]]; then
    WORKSPACE=`pwd`
fi

case "${OSTYPE}" in
    darwin*)  OS="macosx"
              PKG_CMD='zip -r --symlinks'
              PKG_TYPE='zip'
              PROP_FILE=${WORKSPACE}/publish.prop
              if [[ ${IOS} == 'true' ]]; then
                  OS="ios"
                  PROP_FILE=${WORKSPACE}/publish_ios.prop
                  if [[ ${EDITION} == 'enterprise' ]]; then
                      release_config="Release_EE"
                      debug_config="Debug_EE"
                      SCHEME_NAME="LiteCore EE framework"
                  else
                      release_config="Release"
                      debug_config="Debug"
                      SCHEME_NAME="LiteCore framework"
                  fi
              fi;;
    linux*)   OS="linux"
              PKG_CMD='tar czf'
              PKG_TYPE='tar.gz'
              PROP_FILE=${WORKSPACE}/publish.prop
              ;;
    *)        echo "unknown: $OSTYPE"
              exit 1;;
esac

if [[ "$EDITION" == "enterprise" ]]; then
    echo "Building enterprise edition (EDITION = enterprise)"
    build_enterprise="ON"
else
    echo "Building community edition (EDITION = $EDITION)"
    build_enterprise="OFF"
fi

ios_xcode_proj="couchbase-lite-core/Xcode/LiteCore.xcodeproj"
macosx_lib="libLiteCore.dylib"

#create artifacts dir for publishing to latestbuild
ARTIFACTS_SHA_DIR=${WORKSPACE}/artifacts/${PRODUCT}/sha/${SHA_VERSION:0:2}/${SHA_VERSION}
ARTIFACTS_BUILD_DIR=${WORKSPACE}/artifacts/${PRODUCT}/${VERSION}/${BLD_NUM}
mkdir -p ${ARTIFACTS_SHA_DIR}
mkdir -p ${ARTIFACTS_BUILD_DIR}

echo SHA_VERSION=${SHA_VERSION}
BIN_NAME="LiteCore"
FRAMEWORK_LOC=${BIN_NAME}.xcarchive/Products/Library/Frameworks/${BIN_NAME}.framework
# Global define end

# this will be used to collect all destination framework path with `-framework`
# to include them in `-create-xcframework`
FRAMEWORK_PATH_ARGS=()

# arg1 = target destination for which the archive is built for. E.g., "generic/platform=iOS"
# arg2 = Configuration for building (Release, Debug_EE, etc)
xcarchive()
{
  DESTINATION=${1}
  CONFIGURATION=${2}
  echo "Archiving for ${DESTINATION}..."
  ARCHIVE_PATH=$PWD/$(echo ${DESTINATION} | sed 's/ /_/g' | sed 's/\//\_/g')
  xcodebuild archive \
    -project "$WORKSPACE/$ios_xcode_proj" \
    -scheme "${SCHEME_NAME}" \
    -configuration "${CONFIGURATION}" \
    -destination "${DESTINATION}" \
    -archivePath "${ARCHIVE_PATH}/${BIN_NAME}.xcarchive" \
    "ONLY_ACTIVE_ARCH=NO" \
    "CODE_SIGNING_ALLOWED=NO" \
    "SKIP_INSTALL=NO"
  
  if [[ ${CONFIGURATION} == Release* ]]; then
    FRAMEWORK_PATH_ARGS+=("-framework "${ARCHIVE_PATH}/${FRAMEWORK_LOC}" \
        -debug-symbols "${ARCHIVE_PATH}/${BIN_NAME}.xcarchive/dSYMs/${BIN_NAME}.framework.dSYM"")
  else
    FRAMEWORK_PATH_ARGS+=("-framework "${ARCHIVE_PATH}/${FRAMEWORK_LOC})
  fi
  echo "Finished archiving ${DESTINATION}."
}

build_xcode_binaries () {
    echo "====  Building ios ${FLAVOR} binary  ==="
    config_name="${FLAVOR}_config"
    mkdir ${WORKSPACE}/build_ios_${FLAVOR}
    pushd ${WORKSPACE}/build_ios_${FLAVOR}
    FRAMEWORK_PATH_ARGS=()
    xcarchive "generic/platform=iOS Simulator" ${!config_name}
    xcarchive "generic/platform=iOS" ${!config_name}
    xcarchive "generic/platform=macOS,variant=Mac Catalyst" ${!config_name}
    xcodebuild -create-xcframework -output "${WORKSPACE}/build_ios_${FLAVOR}/${BIN_NAME}.xcframework" ${FRAMEWORK_PATH_ARGS[*]}
    popd
}

build_binaries () {
    CMAKE_BUILD_TYPE_NAME="cmake_build_type_${FLAVOR}"
    mkdir -p ${WORKSPACE}/build_${FLAVOR}
    pushd ${WORKSPACE}/build_${FLAVOR}
    if [[ ${OS} == 'linux' ]]; then
      cmake -DBUILD_ENTERPRISE=$build_enterprise -DEMBEDDED_MDNS=ON -DCMAKE_INSTALL_PREFIX=`pwd`/install -DCMAKE_BUILD_TYPE=${!CMAKE_BUILD_TYPE_NAME} -DLITECORE_MACOS_FAT_DEBUG=ON -DVERSION=${VERSION} -DBLD_NUM=${BLD_NUM} ../couchbase-lite-core
    else
      cmake -DBUILD_ENTERPRISE=$build_enterprise -DCMAKE_INSTALL_PREFIX=`pwd`/install -DCMAKE_BUILD_TYPE=${!CMAKE_BUILD_TYPE_NAME} -DLITECORE_MACOS_FAT_DEBUG=ON -DVERSION=${VERSION} -DBLD_NUM=${BLD_NUM} ../couchbase-lite-core
    fi
    make -j8
    if [[ ${OS} == 'linux' ]]; then
        ${WORKSPACE}/couchbase-lite-core/build_cmake/scripts/strip.sh $PWD
    else
        dsymutil ${macosx_lib} -o libLiteCore.dylib.dSYM
        strip -x ${macosx_lib}
    fi
    make install
    if [[ ${OS} == 'macosx' ]]; then
        # package up the strip symbols
        cp -rp libLiteCore.dylib.dSYM  ./install/lib
    else
        cxx=${CXX:-g++}
        cc=${CC:-gcc}
        # copy C++ stdlib, etc to output
        echo "Copying libs from compiler"
        libstdcpp=`$cxx --print-file-name=libstdc++.so`
        libstdcppname=`basename "$libstdcpp"`
        libgcc_s=`$cc --print-file-name=libgcc_s.so.1`
        libgcc_sname=`basename "$libgcc_s"`

        cp -p "$libstdcpp" "./install/lib/$libstdcppname" -v
        ln -s "./install/lib/${libstdcppname}.6" "$libstdcppname" -v
        cp -p "${libgcc_s}" "./install/lib" -v
    fi
    if [[ -z ${SKIP_TESTS} ]] && [[ ${EDITION} == 'enterprise' ]]; then
        chmod 777 ${WORKSPACE}/couchbase-lite-core/build_cmake/scripts/test_unix.sh
        cd ${WORKSPACE}/build_${FLAVOR}/ && ${WORKSPACE}/couchbase-lite-core/build_cmake/scripts/test_unix.sh
    fi
    popd
}

create_pkgs () {
    # Create zip package
    for FLAVOR in release debug
    do
        PACKAGE_NAME=${PRODUCT}-${OS}-${SHA_VERSION}-${FLAVOR}.${PKG_TYPE}
        SYMBOLS_PKG_NAME=${PRODUCT}-${OS}-${SHA_VERSION}-${FLAVOR}-symbols.${PKG_TYPE}
        echo
        echo  "=== Creating ${WORKSPACE}/${PACKAGE_NAME} package ==="
        echo

        if [[ ${OS} == 'ios' ]]; then
            pushd ${WORKSPACE}/build_ios_${FLAVOR}
            ${PKG_CMD} ${WORKSPACE}/${PACKAGE_NAME} LiteCore.xcframework
            popd
        else
            pushd ${WORKSPACE}/build_${FLAVOR}/install
            # Create separate symbols pkg
            if [[ ${OS} == 'macosx' ]]; then
                ${PKG_CMD} ${WORKSPACE}/${PACKAGE_NAME} lib/libLiteCore*.dylib include
                ${PKG_CMD} ${WORKSPACE}/${SYMBOLS_PKG_NAME}  lib/libLiteCore.dylib.dSYM
            else # linux
                ${PKG_CMD} ${WORKSPACE}/${PACKAGE_NAME} *
                cd ${WORKSPACE}/build_${FLAVOR}/
                ${PKG_CMD} ${WORKSPACE}/${SYMBOLS_PKG_NAME} libLiteCore*.sym
            fi
            popd
        fi
    done
}

create_prop () {
    # Create publishing prop file
    pushd ${WORKSPACE}
    echo "PRODUCT=${PRODUCT}"  >> ${PROP_FILE}
    echo "BLD_NUM=${BLD_NUM}"  >> ${PROP_FILE}
    echo "VERSION=${SHA_VERSION}" >> ${PROP_FILE}
    echo "PKG_TYPE=${PKG_TYPE}" >> ${PROP_FILE}
    if [[ ${OS} == 'ios' ]]; then
        echo "DEBUG_IOS_PKG_NAME=${PRODUCT}-${OS}-${SHA_VERSION}-debug.${PKG_TYPE}" >> ${PROP_FILE}
        echo "RELEASE_IOS_PKG_NAME=${PRODUCT}-${OS}-${SHA_VERSION}-release.${PKG_TYPE}" >> ${PROP_FILE}
    else
        echo "DEBUG_PKG_NAME=${PRODUCT}-${OS}-${SHA_VERSION}-debug.${PKG_TYPE}" >> ${PROP_FILE}
        echo "RELEASE_PKG_NAME=${PRODUCT}-${OS}-${SHA_VERSION}-release.${PKG_TYPE}" >> ${PROP_FILE}
        echo "SYMBOLS_DEBUG_PKG_NAME=${PRODUCT}-${OS}-${SHA_VERSION}-debug-symbols.${PKG_TYPE}" >> ${PROP_FILE}
        echo "SYMBOLS_RELEASE_PKG_NAME=${PRODUCT}-${OS}-${SHA_VERSION}-release-symbols.${PKG_TYPE}" >> ${PROP_FILE}
    fi

    echo
    echo  "=== Created ${WORKSPACE}/${PROP_FILE} ==="
    echo

    cat ${PROP_FILE}

    popd
}

prep_artifacts () {
    # Prepare artifact directory for latestbuild publishing

    # Nexus strips "release" from ${PRODUCT}-${OS}-${SHA_VERSION}-release.${PKG_TYPE}.  It also transforms
    # ${PRODUCT}-${OS}-${SHA_VERSION}-<debug|release>-symbols.${PKG_TYPE} to
    # ${PRODUCT}-${OS}-${SHA_VERSION}-symbols-<release|debug>.${PKG_TYPE}.  In order to minic this on
    # latestbuild, these pkgs are renamed accordingly during copy

    cp ${WORKSPACE}/${PRODUCT}-${OS}-${SHA_VERSION}-debug.${PKG_TYPE} ${ARTIFACTS_SHA_DIR}/${PRODUCT}-${OS}-debug.${PKG_TYPE}
    cp ${WORKSPACE}/${PRODUCT}-${OS}-${SHA_VERSION}-release.${PKG_TYPE} ${ARTIFACTS_SHA_DIR}/${PRODUCT}-${OS}.${PKG_TYPE}

    cp ${WORKSPACE}/${PRODUCT}-${OS}-${SHA_VERSION}-debug.${PKG_TYPE} ${ARTIFACTS_BUILD_DIR}/${PRODUCT}-${EDITION}-${VERSION}-${BLD_NUM}-${OS}-debug.${PKG_TYPE}
    cp ${WORKSPACE}/${PRODUCT}-${OS}-${SHA_VERSION}-release.${PKG_TYPE} ${ARTIFACTS_BUILD_DIR}/${PRODUCT}-${EDITION}-${VERSION}-${BLD_NUM}-${OS}.${PKG_TYPE}

    if [[ ${OS} != 'ios' ]]; then
        cp ${WORKSPACE}/${PRODUCT}-${OS}-${SHA_VERSION}-debug-symbols.${PKG_TYPE}  ${ARTIFACTS_SHA_DIR}/${PRODUCT}-${OS}-symbols-debug.${PKG_TYPE}
        cp ${WORKSPACE}/${PRODUCT}-${OS}-${SHA_VERSION}-release-symbols.${PKG_TYPE} ${ARTIFACTS_SHA_DIR}/${PRODUCT}-${OS}-symbols.${PKG_TYPE}

        cp ${WORKSPACE}/${PRODUCT}-${OS}-${SHA_VERSION}-debug-symbols.${PKG_TYPE}  ${ARTIFACTS_BUILD_DIR}/${PRODUCT}-${EDITION}-${VERSION}-${BLD_NUM}-${OS}-symbols-debug.${PKG_TYPE}
        cp ${WORKSPACE}/${PRODUCT}-${OS}-${SHA_VERSION}-release-symbols.${PKG_TYPE} ${ARTIFACTS_BUILD_DIR}/${PRODUCT}-${EDITION}-${VERSION}-${BLD_NUM}-${OS}-symbols.${PKG_TYPE}
    fi
}


#Main 
if [[ ${OS} == 'ios' ]]; then
    echo "====  Building ios Release binary  ==="
    for FLAVOR in release debug; do
        build_xcode_binaries
    done
else
    echo "====  Building macosx/linux Release binary  ==="
    cmake_build_type_release=MinSizeRel
    cmake_build_type_debug=Debug
    for FLAVOR in release debug; do
        build_binaries
    done
fi

create_pkgs

create_prop

prep_artifacts

