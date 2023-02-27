#!/bin/sh

DIRS=("C" "Crypto" "LiteCore" "Networking" "Replicator" "REST")

for dir in ${DIRS[@]}; do
  find $dir/. -iname '*.hh' -o -iname '*.cc' -o -iname '*.h' -o -iname '*.c' | xargs clang-format -i
done
