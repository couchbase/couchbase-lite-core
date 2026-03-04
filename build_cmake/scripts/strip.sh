#!/bin/bash

# Copyright 2017-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

set -e

PREFIX=""
WORKING_DIR="${1}"

if [[ $# > 1 ]]; then
  PREFIX="${2}"
fi
OBJCOPY=${OBJCOPY:-${PREFIX}objcopy}
STRIP=${STRIP:-${PREFIX}strip}

if [[ $# > 0 ]]; then 
  pushd $WORKING_DIR
fi

# From https://sourceware.org/gdb/current/onlinedocs/gdb.html/MiniDebugInfo.html

# Extract the dynamic symbols from the main binary, there is no need
# to also have these in the normal symbol table.
nm -D libLiteCore.so --format=posix --defined-only \
  | awk '{ print $1 }' | sort > dynsyms

# Extract all the text (i.e. function) symbols from the debuginfo.
# (Note that we actually also accept "D" symbols, for the benefit
# of platforms like PowerPC64 that use function descriptors.)
nm libLiteCore.so --format=posix --defined-only \
  | awk '{ if ($2 == "T" || $2 == "t" || $2 == "D") print $1 }' \
  | sort > funcsyms

# Keep all the function symbols not already in the dynamic symbol
# table.
comm -13 dynsyms funcsyms > keep_symbols
rm dynsyms funcsyms

# Separate full debug info into debug binary.
$OBJCOPY --only-keep-debug libLiteCore.so libLiteCore.so.sym

# Copy the full debuginfo, keeping only a minimal set of symbols and
# removing some unnecessary sections.
$OBJCOPY -S --remove-section .gdb_index --remove-section .comment \
  --keep-symbols=keep_symbols libLiteCore.so.sym libLiteCore.so.minisym

# Drop the full debug info from the original binary.
$STRIP --strip-debug --strip-unneeded -R .comment libLiteCore.so

# Inject the compressed data into the .gnu_debugdata section of the
# original binary.
xz libLiteCore.so.minisym
$OBJCOPY --add-section .gnu_debugdata=libLiteCore.so.minisym.xz libLiteCore.so
$OBJCOPY --add-gnu-debuglink=libLiteCore.so.sym libLiteCore.so
rm libLiteCore.so.minisym.xz keep_symbols
