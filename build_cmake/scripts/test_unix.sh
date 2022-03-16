#!/bin/bash -e

# Copyright 2017-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

# Note this script is used by the build server so this script cannot rely
# on any sort of directory structure. It must be run from the root directory
# of the LiteCore CMake output
if [ ! -f LiteCore/tests/CppTests ]; then
  echo "CppTests not found!"
  exit 1
fi

if [ ! -f C/tests/C4Tests ]; then
  echo "C4Tests not found!"
  exit 1
fi

pushd LiteCore/tests
LiteCoreTestsQuiet=1 ./CppTests -r quiet
popd

pushd C/tests
LiteCoreTestsQuiet=1 ./C4Tests -r quiet
popd
