#!/usr/bin/env bash

set -e

node --experimental-wasm-eh build/C/tests/C4Tests.js -r list
node --experimental-wasm-eh build/LiteCore/tests/CppTests.js  -r list
