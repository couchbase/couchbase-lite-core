# This script creates a C header "repo_version.h" with string constants that
# contain the current Git status of the repo.
# It takes a single parameter, the path of the file to generate.
Param(
    [Parameter(Mandatory=$True,Position=1)][string]$outputPath,
    [Parameter()][string]$gitPath = "C:\Program Files\Git\bin\git.exe"
)

$Official="false"
if((Test-Path env:BLD_NUM) -And (Test-Path env:VERSION)) {
  $GitBranch=""
  $GitCommit=$env:VERSION
  $GitDirty=""
  $BuildNum=$env:BLD_NUM
  $Official="true"
} else {
  $GitBranch = Invoke-Expression '& $gitPath rev-parse --symbolic-full-name HEAD | ForEach-Object { $_ -replace "refs/heads/","" }'
  $GitCommit = Invoke-Expression '& $gitPath rev-parse HEAD'
  Invoke-Expression '& $gitPath status --porcelain' > $null
  if($?) {
    $GitDirty="+CHANGES"
  } else {
    $GitDirty = ""
  }
  $BuildNum=""
}

$outContent = @"
#define GitCommit `"$GitCommit`"
#define GitBranch `"$GitBranch`"
#define GitDirty  `"$GitDirty`"
#define LiteCoreBuildNum `"$BuildNum`"
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
