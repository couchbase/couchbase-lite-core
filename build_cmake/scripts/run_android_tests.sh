#/bin/sh

export TMPDIR=/data/local/tmp
export LD_LIBRARY_PATH=`pwd`
./CppTests -r list "exclude:Collatable RandomFloats"
./C4Tests -r list
