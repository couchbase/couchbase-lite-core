#!/bin/sh
# Runs clang-format on each of the below LiteCore directories
# Must be ran from Core root directory ('couchbase-lite-core/')
# Formatting is based off the .clang-format file in the root of Core

DIRS=("C" "Crypto" "LiteCore" "Networking" "Replicator" "REST")

for dir in ${DIRS[@]}; do
  find $dir/. -iname '*.hh' -o -iname '*.cc' -o -iname '*.h' -o -iname '*.c' | xargs clang-format -i
done
