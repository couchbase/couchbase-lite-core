#!/bin/bash

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ ! -z $CHANGE_TARGET ]; then
    BRANCH=$CHANGE_TARGET
fi

if [ -z $WORKSPACE ]; then
    WORKSPACE="$SCRIPT_DIR/.."
fi

if ! [ -x "$(command -v git)" ]; then
  echo 'Error: git is not installed.' >&2
  exit 1
fi

# Jenkins is a pain because it doesn't give the option for no source
# and junctions make Git blow up so the easiest way is to just clone
# the whole freaking thing again
COMMIT_SHA=`git rev-parse HEAD`
if [ -d "$WORKSPACE/couchbase-lite-core" ]; then
    pushd "$WORKSPACE/couchbase-lite-core"
    git fetch origin
    git reset --hard
    git checkout $COMMIT_SHA
    git clean -dfx .
    popd
else
    git clone ssh://git@github.com/couchbase/couchbase-lite-core $WORKSPACE/couchbase-lite-core
    pushd "$WORKSPACE/couchbase-lite-core"
    git checkout $COMMIT_SHA
    git submodule update --init --recursive
    popd
fi

if [ -d "$WORKSPACE/couchbase-lite-core-EE" ]; then
    pushd "$WORKSPACE/couchbase-lite-core-EE"
    git fetch origin
    git reset --hard
    git checkout $BRANCH
    git clean -dfx .
    git pull origin $BRANCH
    popd
else
    git clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $BRANCH --recursive "$WORKSPACE/couchbase-lite-core-EE"
fi

ulimit -c unlimited # Enable crash dumps
mkdir -p "$WORKSPACE/couchbase-lite-core/build_cmake/x64"
pushd "$WORKSPACE/couchbase-lite-core/build_cmake/x64"
cmake -DBUILD_ENTERPRISE=ON ../..
make -j8
pushd LiteCore/tests
LiteCoreTestsQuiet=1 ./CppTests -r list
popd

pushd C/tests
LiteCoreTestsQuiet=1 ./C4Tests -r list
popd

popd
