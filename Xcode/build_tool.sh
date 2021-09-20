#!/bin/bash

# Copyright 2017-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

set -e

function usage 
{
  echo "$0: Builds LiteCore tools for macOS"
  echo "Usage: ${0} -t <ToolName> -o <Output Directory> [-clean] [-v <Version (<Version Number>[-<Build Number>])>]"
  echo "Current directory should be couchbase-lite-core/Xcode/"
}

CLEAN=""

while [[ $# -gt 1 ]]
do
  key=${1}
  case $key in
    -t)
      SCHEME="${2} tool"
      shift
      ;;
    -clean)
      CLEAN="clean"
      ;;
    -o)
      OUTPUT_DIR="${2}"
      shift
      ;;
    -v)
      VERSION="${2}"
      shift
      ;;
    *)
      usage
      exit 3
      ;;
  esac
  shift
done

if [ -z "$SCHEME" ] || [ -z "$OUTPUT_DIR" ]
then
  usage
  exit 4
fi

echo "Scheme: ${SCHEME}"
echo "Output Directory: ${OUTPUT_DIR}"
echo ""

# Set the build version configuration:
BUILD_VERSION=""
BUILD_NUMBER=""
if [ ! -z "$VERSION" ]
then
  IFS='-' read -a VERSION_ITEMS <<< "${VERSION}"
  if [[ ${#VERSION_ITEMS[@]} > 1 ]]
  then
    BUILD_VERSION="CBL_VERSION_STRING=${VERSION_ITEMS[0]}"
    BUILD_NUMBER="CBL_BUILD_NUMBER=${VERSION_ITEMS[1]}"
  else
    BUILD_VERSION="CBL_VERSION_STRING=${VERSION}"
  fi
fi

# Build!
xcodebuild -scheme "${SCHEME}" -configuration Release ${BUILD_VERSION} ${BUILD_NUMBER} "CODE_SIGNING_REQUIRED=NO" "CODE_SIGN_IDENTITY=" $CLEAN build

# Find where Xcode put the tool:
PRODUCTS_DIR=`xcodebuild -scheme "${SCHEME}" -configuration Release -showBuildSettings|grep -w BUILT_PRODUCTS_DIR|head -n 1|awk '{ print $3 }'`
BIN_NAME=`xcodebuild -scheme "${SCHEME}" -configuration Release -showBuildSettings|grep -w PRODUCT_NAME|head -n 1|awk '{ print $3 }'`
SOURCE="${PRODUCTS_DIR}/${BIN_NAME}"

# Copy the binary:
DESTINATION="${OUTPUT_DIR}/${BIN_NAME}"
cp "${SOURCE}" "${DESTINATION}"

echo "Your tool is ready: ${DESTINATION}"
