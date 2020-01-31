#!/bin/bash

set -e
shopt -s extglob dotglob

function build_xcode {
    pushd "couchbase-lite-core/Xcode"
    xcodebuild -project LiteCore.xcodeproj -configuration Debug-EE -derivedDataPath ios -scheme "LiteCore framework" -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO
    popd
}


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

unameOut="$(uname -s)"
case "${unameOut}" in
    # Build XCode project on mac because it has stricter warnings
    Darwin*)    
        build_xcode
        if [[ -z "$KEYCHAIN_PWD" ]]; then
            echo "Keychain credentials not found, aborting..."
            exit 1
        fi
        
        security -v unlock-keychain -p $KEYCHAIN_PWD $HOME/Library/Keychains/login.keychain-db
        ;;
esac

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
