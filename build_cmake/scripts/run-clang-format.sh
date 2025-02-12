#!/bin/sh
# Runs clang-format on each of the below LiteCore directories
# Must be ran from Core root directory ('couchbase-lite-core/')
# Formatting is based off the .clang-format file in the root of Core
# clang-format should either be installed by homebrew, or available to /bin/sh's path
PATH=$PATH:/opt/homebrew/bin

DIRS=("C" "Crypto" "LiteCore" "MSVC" "Networking" "Replicator" "REST" "tool_support")

CURDIR=$(pwd)

# Move back to Core's root if this is being ran from Xcode
if [[ ${CURDIR} == */Xcode ]]; then
    cd ..
fi


for dir in ${DIRS[@]}; do
  find $dir/. \( -iname '*.hh' -o -iname '*.cc' -o -iname '*.h' -o -iname '*.c' -o -iname '*.cpp' \) -a \! -iname 'n1ql.cc' | xargs clang-format -i
done
