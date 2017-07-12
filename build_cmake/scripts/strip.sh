#!/bin/bash

PREFIX=""
WORKING_DIR="${1}"

if [[ $# > 1 ]]; then
  PREFIX="${2}"
fi

pushd $WORKING_DIR
COMMAND="find . -name \"*.a\" | xargs ${PREFIX}strip --strip-unneeded"
eval ${COMMAND}
rm libLiteCore.so
make -j8 LiteCore
COMMAND="${PREFIX}objcopy --only-keep-debug libLiteCore.so libLiteCore.so.sym"
eval ${COMMAND}
COMMAND="${PREFIX}strip --strip-unneeded libLiteCore.so"
eval ${COMMAND}
COMMAND="${PREFIX}objcopy --add-gnu-debuglink=libLiteCore.so.sym libLiteCore.so"
eval ${COMMAND}
popd
