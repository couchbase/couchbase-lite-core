#!/bin/bash

# Copyright 2020-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

set -e
shopt -s extglob dotglob

script_dir=`dirname $0`

if ! [ -x "$(command -v git)" ]; then
  echo 'Error: git is not installed.' >&2
  exit 1
fi

if [[ -z "$KEYCHAIN_PWD" ]]; then
    echo "Keychain credentials not found, aborting..."
    exit 1
fi

security -v unlock-keychain -p $KEYCHAIN_PWD $HOME/Library/Keychains/login.keychain-db

mkdir "couchbase-lite-core"
git submodule update --init --recursive
mv !(couchbase-lite-core) couchbase-lite-core

# Sometimes a PR depends on a PR in the EE repo as well.  This needs to be convention based, so if there is a branch with the name PR-###
# (with the GH PR number) in the EE repo then use that, otherwise use the name of the target branch (master, release/XXX etc)
git clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $BRANCH_NAME --recursive --depth 1 couchbase-lite-core-EE || \
    git clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $CHANGE_TARGET --recursive --depth 1 couchbase-lite-core-EE 

pushd "couchbase-lite-core/Xcode"
xcodebuild -project LiteCore.xcodeproj -configuration Debug_EE -derivedDataPath ios-sim -scheme "LiteCore framework" -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO
xcodebuild -project LiteCore.xcodeproj -configuration Debug_EE -derivedDataPath ios -scheme "LiteCore framework" -sdk iphoneos BITCODE_GENERATION_MODE=bitcode CODE_SIGNING_ALLOWED=NO
