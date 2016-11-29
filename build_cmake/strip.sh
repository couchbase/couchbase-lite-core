#!/bin/bash

echo "-d" > stripopts
while read line; do
  if [[ "$line" != "" && "${line:0:1}" != "#" ]]; then
    echo "-K ${line:1}" >> stripopts
  fi
done < ../C/c4.exp

echo "libLiteCore.so" >> stripopts
strip @stripopts
rm stripopts
