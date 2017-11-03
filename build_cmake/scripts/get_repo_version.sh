#!/bin/bash -e

# This script creates a C header "repo_version.h" with string constants that
# contain the current Git status of the repo.
# It takes a single parameter, the path of the file to generate.

OUTPUT_FILE="$1"
OFFICIAL=false
if [[ ! -z "$BLD_NUM" ]] && [[ ! -z "$VERSION" ]]; then
  GIT_BRANCH=""
  GIT_COMMIT=$VERSION
  GIT_DIRTY=""
  BUILD_NUM=$BLD_NUM
  OFFICIAL=true
else
  GIT_BRANCH=`git rev-parse --symbolic-full-name HEAD | sed -e 's/refs\/heads\///'`
  GIT_COMMIT=`git rev-parse HEAD || true`
  GIT_DIRTY=$(test -n "`git status --porcelain`" && echo "+CHANGES" || true)
  BUILD_NUM=""
fi

echo "#define GitCommit \"$GIT_COMMIT\"" $'\n'\
     "#define GitBranch \"$GIT_BRANCH\"" $'\n'\
     "#define GitDirty  \"$GIT_DIRTY\"" $'\n'\
     "#define LiteCoreBuildNum \"$BUILD_NUM\"" $'\n'\
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
