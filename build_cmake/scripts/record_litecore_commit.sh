#!/bin/bash

# Copyright 2019-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

 function print_help {
     echo "Usage: record_litecore_commit.sh <ARGUMENTS>"
     echo
     echo "ARGUMENTS"
     echo "--user       |-u    FTP username for upload"
     echo "--pass       |-p    FTP password for upload"
     echo "--lite-branch|-b    The branch of Couchbase Lite to record the commit for"
 }

USER=""
PASS=""
LITE_BRANCH=""

while (( "$#" )); do
  case "$1" in
    -u|--user)
      USER=$2
      shift 2
      ;;
    -p|--pass)
      PASS=$2
      shift 2
      ;;
    -b|--lite-branch)
      LITE_BRANCH=$2
      shift 2
      ;;
    -h|--help)
      print_help
      exit 0
      ;;
    --) # end argument parsing
      shift
      break
      ;;
    -*|--*=) # unsupported flags
      echo "Error: Unsupported flag $1" >&2
      print_help
      exit 1
      ;;
    *)
      echo "Error: Position arguments not allowed ($1 given)" >&2
      print_help
      exit 1
      ;;
  esac
done

if [[ -z $USER ]]; then
    echo "No username provided..."
    print_help
    exit 1
fi

if [[ -z $PASS ]]; then
    echo "No password provided..."
    print_help
    exit 2
fi

if [[ -z $LITE_BRANCH ]]; then
    echo "No Couchbase Lite branch provided..."
    print_help
    exit 3
fi

SUBMODULE_SHA=`git rev-parse HEAD`
FILENAME_PREFIX=`echo $LITE_BRANCH | sed -e 's/\//_/'`
echo $SUBMODULE_SHA > $FILENAME_PREFIX.txt
ftp -n latestbuilds.service.couchbase.com <<END_SCRIPT
quote USER $USER
quote PASS $PASS
binary
cd builds/latestbuilds/couchbase-lite-core
put $FILENAME_PREFIX.txt
quit
END_SCRIPT
rm $FILENAME_PREFIX.txt
