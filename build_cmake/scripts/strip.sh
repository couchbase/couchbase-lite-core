#!/bin/bash

# Copyright 2017-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

PREFIX=""
WORKING_DIR="${1}"

if [[ $# > 1 ]]; then
  PREFIX="${2}"
fi

pushd $WORKING_DIR
COMMAND="${PREFIX}objcopy --only-keep-debug libLiteCore.so tmp"
eval ${COMMAND}
COMMAND="find . -name \"*.a\" | xargs ${PREFIX}strip --strip-unneeded"
eval ${COMMAND}
rm libLiteCore.so*
mv tmp libLiteCore.so.sym
make -j8 LiteCore
COMMAND="${PREFIX}strip --strip-unneeded libLiteCore.so"
eval ${COMMAND}
COMMAND="${PREFIX}objcopy --add-gnu-debuglink=libLiteCore.so.sym libLiteCore.so"
eval ${COMMAND}
popd
