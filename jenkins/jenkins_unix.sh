#!/bin/bash

set -e
shopt -s extglob dotglob

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

if [ ! -z $CHANGE_TARGET ]; then
    BRANCH=$CHANGE_TARGET
fi

if ! [ -x "$(command -v git)" ]; then
  echo 'Error: git is not installed.' >&2
  exit 1
fi

mkdir "couchbase-lite-core"
git submodule update --init --recursive
mv !(couchbase-lite-core) couchbase-lite-core
git clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $BRANCH --recursive --depth 1 couchbase-lite-core-EE

ulimit -c unlimited # Enable crash dumps
mkdir -p "couchbase-lite-core/build_cmake/x64"
pushd "couchbase-lite-core/build_cmake/x64"
cmake -DBUILD_ENTERPRISE=ON ../..
make -j8
pushd LiteCore/tests
LiteCoreTestsQuiet=1 ./CppTests -r list
popd

pushd C/tests
LiteCoreTestsQuiet=1 ./C4Tests -r list
