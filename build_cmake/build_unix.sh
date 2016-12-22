#!/bin/bash

core_count=`getconf _NPROCESSORS_ONLN`
CC=clang CXX=clang++ cmake -DCMAKE_BUILD_TYPE=Release ..
make -j `expr $core_count + 1`
