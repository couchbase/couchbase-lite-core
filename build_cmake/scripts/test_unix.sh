#!/bin/bash -e

if [ ! -f LiteCore/tests/CppTests ]; then
  echo "CppTests not found!"
  exit 1
fi

if [ ! -f C/tests/C4Tests ]; then
  echo "C4Tests not found!"
  exit 1
fi

pushd LiteCore/tests
./CppTests -r list
popd

pushd C/tests
./C4Tests -r list
popd
