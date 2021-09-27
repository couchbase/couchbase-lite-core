# Copyright 2017-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

# This script creates a C header "repo_version.h" with string constants that
# contain the current Git status of the repo.
# It takes a single parameter, the path of the file to generate.
Param(
    [Parameter(Mandatory=$True,Position=1)][string]$outputPath,
    [Parameter()][string]$gitPath = "C:\Program Files\Git\bin\git.exe"
)

# To fake an official build for testing purposes, uncomment these two lines:
# $env:BLD_NUM=54321
# $env:VERSION="abcdefabcdef"

$Official="false"
$Top = (Invoke-Expression '& $gitPath rev-parse --show-toplevel').Replace("/","\\");
$GitCommitEE=""

# Windows is not as clever about links as Unix is
if(Test-Path $Top\..\..\..\couchbase-lite-core-EE\) {
    Push-Location $Top\..\..\..\couchbase-lite-core-EE\
    $GitCommitEE = Invoke-Expression '& $gitPath rev-parse HEAD'
    Pop-Location
} elseif(Test-Path $Top\..\couchbase-lite-core-EE\) {
    Push-Location $Top\..\couchbase-lite-core-EE\
    $GitCommitEE = Invoke-Expression '& $gitPath rev-parse HEAD'
    Pop-Location
}

$GitCommit = Invoke-Expression '& $gitPath rev-parse HEAD'
if((Test-Path env:BLD_NUM) -And (Test-Path env:VERSION)) {
  $GitBranch=""
  $GitDirty=""
  $BuildNum=$env:BLD_NUM
  $Official="true"
  $Version = $env:VERSION
} else {
  $GitBranch = Invoke-Expression '& $gitPath rev-parse --symbolic-full-name HEAD | ForEach-Object { $_ -replace "refs/heads/","" }'
  Invoke-Expression '& $gitPath status --porcelain' > $null
  if($?) {
    $GitDirty="+CHANGES"
  } else {
    $GitDirty = ""
  }
  $BuildNum=""
  $Version="0.0.0"
}

if(!$env:LITECORE_VERSION_STRING) {
    $env:LITECORE_VERSION_STRING=$Version
}

$outContent = @"
#define GitCommit `"$GitCommit`"
#define GitCommitEE `"$GitCommitEE`"
#define GitBranch `"$GitBranch`"
#define GitDirty  `"$GitDirty`"
#define LiteCoreVersion `"$env:LITECORE_VERSION_STRING`"
#define LiteCoreBuildNum `"$BuildNum`"
#define LiteCoreBuildID `"$Version`"
#define LiteCoreOfficial $Official
"@

echo $outContent | Out-File -FilePath "${outputPath}.tmp" -Force
New-Item $outputPath -ItemType File -ErrorAction Ignore
$existingContent = Get-Content $outputPath
if($existingContent -eq $null) {
    $existingContent = ""
}

if($(Compare-Object $(Get-Content "${outputPath}.tmp") $existingContent).Count) {
    Move-Item -Force "${outputPath}.tmp" "$outputPath"
    echo "get_repo_version.ps1: Updated $outputPath"
} else {
    rm "${outputPath}.tmp"
} 
