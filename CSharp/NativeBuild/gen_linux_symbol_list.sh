#!/bin/bash

echo "-s" > stripopts
while read line; do
  if [[ "$line" != "" && "${line:0:1}" != "#" ]]; then
    echo "-K ${line:1}" >> stripopts
  fi
done < ../../C/c4.exp

echo "libCBForest-Interop.so" >> stripopts
