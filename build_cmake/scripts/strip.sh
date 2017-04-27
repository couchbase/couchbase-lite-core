#!/bin/bash

SCRIPT_DIR=`dirname $0`
PREFIX=""
TOP=$SCRIPT_DIR/../..
if [[ $# > 0 ]]; then
  TOP=$SCRIPT_DIR/../../../..
  PREFIX="${1}"
fi

echo "-s" > stripopts
while read line; do
  if [[ "$line" != "" && "${line:0:1}" != "#" ]]; then
    echo "-K ${line:1}" >> stripopts
  fi
done < $TOP/C/c4.exp

echo "libLiteCore.so" >> stripopts

COMMAND="${PREFIX}strip @stripopts"
eval ${COMMAND}
rm stripopts

echo "-s" > stripopts
while read line; do
  if [[ "$line" != "" && "${line:0:1}" != "#" ]]; then
    echo "-K ${line:1}" >> stripopts
  fi
done < $TOP/REST/c4REST.exp

echo "libLiteCoreREST.so" >> stripopts

COMMAND="${PREFIX}strip @stripopts"
eval ${COMMAND}
rm stripopts
