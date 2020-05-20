#!/bin/bash

set -e
shopt -s extglob dotglob

if ! [ -x "$(command -v git)" ]; then
  echo 'Error: git is not installed.' >&2
  exit 1
fi

mkdir "couchbase-lite-core"
git submodule update --init --recursive
mv !(couchbase-lite-core) couchbase-lite-core

# Sometimes a PR depends on a PR in the EE repo as well.  This needs to be convention based, so if there is a branch with the name PR-###
# (with the GH PR number) in the EE repo then use that, otherwise use the name of the target branch (master, release/XXX etc)
git clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $BRANCH_NAME --recursive --depth 1 couchbase-lite-core-EE || \
    git clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $CHANGE_TARGET --recursive --depth 1 couchbase-lite-core-EE 

ulimit -c unlimited # Enable crash dumps
mkdir -p "couchbase-lite-core/build_cmake/x64"
pushd "couchbase-lite-core/build_cmake/x64"
cmake -DBUILD_ENTERPRISE=ON ../..
make -j8

# Note only for macOS
export MallocScribble=1
export MallocDebugReport=stderr

export LiteCoreTestsQuiet=1

pushd LiteCore/tests
./CppTests -r list
popd

pushd C/tests
./C4Tests -r list
