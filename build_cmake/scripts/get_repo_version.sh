#!/bin/bash -e

# This script creates a C header "repo_version.h" with string constants that
# contain the current Git status of the repo, and the official build number/ID (if any).
# It takes a single parameter, the path of the file to generate.

# To fake an official build for testing purposes, uncomment these two lines:
#BLD_NUM=54321
#VERSION=abcdefabcdef

OUTPUT_FILE="$1"
OFFICIAL=false
GIT_COMMIT=`git rev-parse HEAD || true`
if [[ ! -z "$BLD_NUM" ]] && [[ ! -z "$VERSION" ]]; then
  GIT_BRANCH=""
  GIT_DIRTY=""
  BUILD_NUM=$BLD_NUM
  OFFICIAL=true
else
  GIT_BRANCH=`git rev-parse --symbolic-full-name HEAD | sed -e 's/refs\/heads\///'`
  GIT_DIRTY=$(test -n "`git status --porcelain`" && echo "+CHANGES" || true)
  BUILD_NUM=""
  VERSION=""
fi

echo "#define GitCommit \"$GIT_COMMIT\"" $'\n'\
     "#define GitBranch \"$GIT_BRANCH\"" $'\n'\
     "#define GitDirty  \"$GIT_DIRTY\"" $'\n'\
     "#define LiteCoreVersion \"$LITECORE_VERSION_STRING\"" $'\n'\
     "#define LiteCoreBuildNum \"$BUILD_NUM\"" $'\n'\
     "#define LiteCoreBuildID \"$VERSION\"" $'\n'\
     "#define LiteCoreOfficial $OFFICIAL" $'\n'\
     >"$OUTPUT_FILE".tmp

if cmp --quiet "$OUTPUT_FILE" "$OUTPUT_FILE".tmp
then
	rm "$OUTPUT_FILE".tmp
#echo "get_repo_version.sh: Leaving $OUTPUT_FILE unchanged"
else
	mv "$OUTPUT_FILE".tmp "$OUTPUT_FILE"
	echo "get_repo_version.sh: Updated $OUTPUT_FILE"
fi
