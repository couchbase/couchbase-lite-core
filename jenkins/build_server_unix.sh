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
BLD_NUM=${2}
VERSION=${3}
EDITION=${4}

if [[ -z "${WORKSPACE}" ]]; then
    WORKSPACE=`pwd`
fi

case "${OSTYPE}" in
    darwin*)  OS="macosx"
              PKG_CMD='zip -r'
              PKG_TYPE='zip'
              PROP_FILE=${WORKSPACE}/publish.prop
              if [[ ${IOS} == 'true' ]]; then
                  OS="ios"
                  PROP_FILE=${WORKSPACE}/publish_ios.prop
                  if [[ ${EDITION} == 'enterprise' ]]; then
                      release_config="Release_EE"
                      debug_config="Debug_EE"
                  else
                      release_config="Release"
                      debug_config="Debug"
                  fi
              fi;;
    linux*)   OS="linux"
              PKG_CMD='tar czf'
              PKG_TYPE='tar.gz'
              PROP_FILE=${WORKSPACE}/publish.prop
              OS_NAME=`lsb_release -is`
              if [[ "$OS_NAME" != "CentOS" ]]; then
                  echo "Error: Unsupported Linux distro $OS_NAME"
                  exit 2
              fi

              OS_VERSION=`lsb_release -rs`
              if [[ $OS_VERSION =~ ^6.* ]]; then
                  OS="centos6"
              elif [[ ! $OS_VERSION =~ ^7.* ]]; then
                  echo "Error: Unsupported CentOS version $OS_VERSION"
                  exit 3
              fi;;
    *)        echo "unknown: $OSTYPE"
              exit 1;;
esac

project_dir=couchbase-lite-core
strip_dir=${project_dir}
ios_xcode_proj="couchbase-lite-core/Xcode/LiteCore.xcodeproj"
macosx_lib="libLiteCore.dylib"

#create artifacts dir for publishing to latestbuild
ARTIFACTS_DIR=${WORKSPACE}/artifacts/${VERSION:0:2}/${VERSION}
mkdir -p ${ARTIFACTS_DIR}

echo VERSION=${VERSION}
# Global define end

build_xcode_binaries () {
    echo "====  Building ios ${FLAVOR} binary  ==="
    config_name="${FLAVOR}_config"
    mkdir ${WORKSPACE}/build_ios_${FLAVOR}
    pushd ${WORKSPACE}/build_ios_${FLAVOR}
    xcodebuild -project "${WORKSPACE}/${ios_xcode_proj}" -configuration ${!config_name} -derivedDataPath ios -scheme "LiteCore framework" -sdk iphoneos BITCODE_GENERATION_MODE=bitcode CODE_SIGNING_ALLOWED=NO
    xcodebuild -project "${WORKSPACE}/${ios_xcode_proj}" -configuration ${!config_name} -derivedDataPath ios -scheme "LiteCore framework" -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO
    cp -R ios/Build/Products/${!config_name}-iphoneos/LiteCore.framework ${WORKSPACE}/build_ios_${FLAVOR}/
    lipo -create ios/Build/Products/${!config_name}-iphoneos/LiteCore.framework/LiteCore ios/Build/Products/${!config_name}-iphonesimulator/LiteCore.framework/LiteCore -output ${WORKSPACE}/build_ios_${FLAVOR}/LiteCore.framework/LiteCore
    popd
}

build_binaries () {
    CMAKE_BUILD_TYPE_NAME="cmake_build_type_${FLAVOR}"
    mkdir -p ${WORKSPACE}/build_${FLAVOR}
    pushd ${WORKSPACE}/build_${FLAVOR}
    cmake -DEDITION=${EDITION} -DCMAKE_INSTALL_PREFIX=`pwd`/install -DCMAKE_BUILD_TYPE=${!CMAKE_BUILD_TYPE_NAME} ..
    make -j8
    if [[ ${OS} == 'linux'  ]] || [[ ${OS} == 'centos6' ]]; then
        ${WORKSPACE}/couchbase-lite-core/build_cmake/scripts/strip.sh ${strip_dir}
    else
        pushd ${project_dir}
        dsymutil ${macosx_lib} -o libLiteCore.dylib.dSYM
        strip -x ${macosx_lib}
        popd
    fi
    make install
    if [[ ${OS} == 'macosx' ]]; then
        # package up the strip symbols
        cp -rp ${project_dir}/libLiteCore.dylib.dSYM  ./install/lib
    else
        # copy C++ stdlib, etc to output
        libstdcpp=`g++ --print-file-name=libstdc++.so`
        libstdcppname=`basename "$libstdcpp"`
        libgcc_s=`gcc --print-file-name=libgcc_s.so`
        libgcc_sname=`basename "$libgcc_s"`

        cp -p "$libstdcpp" "./install/lib/$libstdcppname"
        ln -s "$libstdcppname" "./install/lib/${libstdcppname}.6"
        cp -p "${libgcc_s}" "./install/lib"
    fi
    if [[ -z ${SKIP_TESTS} ]] && [[ ${EDITION} == 'enterprise' ]]; then
        chmod 777 ${WORKSPACE}/couchbase-lite-core/build_cmake/scripts/test_unix.sh
        cd ${WORKSPACE}/build_${FLAVOR}/${project_dir} && ${WORKSPACE}/couchbase-lite-core/build_cmake/scripts/test_unix.sh
    fi
    popd
}

create_pkgs () {
    # Create zip package
    for FLAVOR in release debug
    do
        PACKAGE_NAME=${PRODUCT}-${OS}-${VERSION}-${FLAVOR}.${PKG_TYPE}
        SYMBOLS_PKG_NAME=${PRODUCT}-${OS}-${VERSION}-${FLAVOR}-symbols.${PKG_TYPE}
        echo
        echo  "=== Creating ${WORKSPACE}/${PACKAGE_NAME} package ==="
        echo

        if [[ ${OS} == 'ios' ]]; then
            pushd ${WORKSPACE}/build_ios_${FLAVOR}
            ${PKG_CMD} ${WORKSPACE}/${PACKAGE_NAME} LiteCore.framework
            popd
        else
            pushd ${WORKSPACE}/build_${FLAVOR}/install
            # Create separate symbols pkg
            if [[ ${OS} == 'macosx' ]]; then
                ${PKG_CMD} ${WORKSPACE}/${PACKAGE_NAME} lib/libLiteCore*.dylib
                ${PKG_CMD} ${WORKSPACE}/${SYMBOLS_PKG_NAME}  lib/libLiteCore.dylib.dSYM
            else # linux
                ${PKG_CMD} ${WORKSPACE}/${PACKAGE_NAME} *
                cd ${WORKSPACE}/build_${FLAVOR}/${strip_dir}
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
    echo "VERSION=${VERSION}" >> ${PROP_FILE}
    echo "PKG_TYPE=${PKG_TYPE}" >> ${PROP_FILE}
    if [[ ${OS} == 'ios' ]]; then
        echo "DEBUG_IOS_PKG_NAME=${PRODUCT}-${OS}-${VERSION}-debug.${PKG_TYPE}" >> ${PROP_FILE}
        echo "RELEASE_IOS_PKG_NAME=${PRODUCT}-${OS}-${VERSION}-release.${PKG_TYPE}" >> ${PROP_FILE}
    else
        echo "DEBUG_PKG_NAME=${PRODUCT}-${OS}-${VERSION}-debug.${PKG_TYPE}" >> ${PROP_FILE}
        echo "RELEASE_PKG_NAME=${PRODUCT}-${OS}-${VERSION}-release.${PKG_TYPE}" >> ${PROP_FILE}
        echo "SYMBOLS_DEBUG_PKG_NAME=${PRODUCT}-${OS}-${VERSION}-debug-symbols.${PKG_TYPE}" >> ${PROP_FILE}
        echo "SYMBOLS_RELEASE_PKG_NAME=${PRODUCT}-${OS}-${VERSION}-release-symbols.${PKG_TYPE}" >> ${PROP_FILE}
    fi

    echo
    echo  "=== Created ${WORKSPACE}/${PROP_FILE} ==="
    echo

    cat ${PROP_FILE}

    popd
}

prep_artifacts () {
    # Prepare artifact directory for latestbuild publishing
    pushd ${ARTIFACTS_DIR}

    # Nexus strips "release" from ${PRODUCT}-${OS}-${VERSION}-release.${PKG_TYPE}.  It also transforms
    # ${PRODUCT}-${OS}-${VERSION}-<debug|release>-symbols.${PKG_TYPE} to 
    # ${PRODUCT}-${OS}-${VERSION}-symbols-<release|debug>.${PKG_TYPE}.  In order to minic this on
    # latestbuild, these pkgs are renamed accordingly during copy

    cp ${WORKSPACE}/${PRODUCT}-${OS}-${VERSION}-debug.${PKG_TYPE} ${PRODUCT}-${OS}-debug.${PKG_TYPE}
    cp ${WORKSPACE}/${PRODUCT}-${OS}-${VERSION}-release.${PKG_TYPE} ${PRODUCT}-${OS}.${PKG_TYPE}

    if [[ ${OS} != 'ios' ]]; then
        cp ${WORKSPACE}/${PRODUCT}-${OS}-${VERSION}-debug-symbols.${PKG_TYPE}  ${PRODUCT}-${OS}-symbols-debug.${PKG_TYPE}
        cp ${WORKSPACE}/${PRODUCT}-${OS}-${VERSION}-release-symbols.${PKG_TYPE} ${PRODUCT}-${OS}-symbols-release.${PKG_TYPE}
    fi
    popd
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

