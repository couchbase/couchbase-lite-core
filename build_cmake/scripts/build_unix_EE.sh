#!/bin/bash

# Copyright 2020-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

set -e

SCRIPT_DIR=`dirname $0`
cd $SCRIPT_DIR/..
mkdir -p unix_EE
cd unix_EE
core_count=`getconf _NPROCESSORS_ONLN`

cmake -DBUILD_ENTERPRISE=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo ../..
make -j `expr $core_count + 1`
