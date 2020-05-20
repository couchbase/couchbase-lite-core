#!/bin/bash -e

SCRIPT_DIR=`dirname $0`
cd $SCRIPT_DIR/../unix

if [ ! -f LiteCore/tests/CppTests ]; then
  echo "CppTests not found!"
  exit 1
fi

if [ ! -f C/tests/C4Tests ]; then
  echo "C4Tests not found!"
  exit 1
fi

pushd LiteCore/tests
LiteCoreTestsQuiet=1 ./CppTests -r list
popd

pushd C/tests
LiteCoreTestsQuiet=1 ./C4Tests -r list
popd
