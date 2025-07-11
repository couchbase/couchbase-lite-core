﻿<#

# Copyright 2020-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

.SYNOPSIS
    A script for the Couchbase official build servers to use to build LiteCore for Windows Desktop and UWP
.DESCRIPTION
    This tool will build various flavors of LiteCore and package them according to the format the the Couchbase build server
    is used to dealing with.  It is the responsibility of the build job to then take the artifacts and put them somewhere.  It
    is meant for the official Couchbase build servers.  Do not try to use it, it will only confuse you.  You have been warned.
.PARAMETER Version
    The version number to give to the build (e.g. 2.0.0)
.PARAMETER BldNum
    The build number to give to the build (e.g. 123)
.PARAMETER ShaVersion
    The commit SHA that this build was built from
.PARAMETER Edition
    The edition to build (community vs enterprise)
.PARAMETER Architectures
    The architectures to build (default Win32, Win64, ARM, ARM64)
.PARAMETER Parallel
    The number of parallel subjobs to allow during the build (default 8)
#>
param(
    [Parameter(Mandatory=$true, HelpMessage="The version number to give to the build (e.g. 2.0.0)")][string]$Version,
    [Parameter(Mandatory=$true, HelpMessage="The build number to give to the build (e.g. 123)")][string]$BldNum,
    [Parameter(Mandatory=$true, HelpMessage="The commit SHA that this build was built from")][string]$ShaVersion,
    [ValidateSet("community", "enterprise")]
    [Parameter(Mandatory=$true, HelpMessage="The edition to build (community vs enterprise)")][string]$Edition,
    [Parameter(HelpMessage="The architectures to build (default Win32, Win32, ARM, ARM64)")][string[]]$Architectures=@("Win32", "Win64", "ARM", "ARM64"),
    [Parameter(HelpMessage="The number of parallel subjobs to allow during the build (default 8)")][int]$Parallel=8
)

$WindowsMinimum = "10.0.19041.0"

function Make-Package() {
    param(
        [Parameter(Mandatory=$true, Position = 0)][string]$directory,
        [Parameter(Mandatory=$true, Position = 1)][string]$filename,
        [Parameter(Mandatory=$true, Position = 2)][string]$architecture,
        [Parameter(Mandatory=$true, Position = 3)][string]$config
    )

    Write-Host "Creating pkg - pkgdir:$directory, pkgname:$filename, arch:$architecture, flavor:$config"
    Push-Location $directory
    & 7za a -tzip -mx9 $env:WORKSPACE\$filename *

    if($LASTEXITCODE -ne 0) {
        throw "Zip failed"
    }

    $PropFile = "$env:WORKSPACE\publish_$arch.prop"
    New-Item -ItemType File -ErrorAction Ignore -Path $PropFile
    Add-Content $PropFile "PRODUCT=couchbase-lite-core"
    Add-Content $PropFile "VERSION=$ShaVersion"
    Add-Content $PropFile "${config}_PKG_NAME_$architecture=$filename"
    Pop-Location
}

function Build() {
    param(
        [Parameter(Mandatory=$true, Position = 0)][string]$directory,
        [Parameter(Mandatory=$true, Position = 1)][string]$architecture,
        [Parameter(Mandatory=$true, Position = 2)][string]$config
    )

    Write-Host "Building blddir:$directory, arch:$architecture, flavor:$config"
    New-Item -ItemType Directory -ErrorAction Ignore $directory
    Push-Location $directory
    $MsArch = $architecture
    if($architecture -eq "Win64") {
        $MsArch = "x64"
    }

    if($Edition -eq "enterprise") {
        $build_enterprise = "ON"
    } else {
        $build_enterprise = "OFF"
    }

    # This -T version controls which version of the MSVC toolchain is used.
    # Once it is decided for a given minor release line, it should not be changed.
    & "C:\Program Files\CMake\bin\cmake.exe" `
        -T version=14.36.17.6 `
        -A $MsArch `
        -DBUILD_ENTERPRISE=$build_enterprise `
        -DCMAKE_INSTALL_PREFIX="$(Get-Location)\install" `
        -S ..\couchbase-lite-core

    if($LASTEXITCODE -ne 0) {
        throw "CMake failed"
    }

    & "C:\Program Files\CMake\bin\cmake.exe" --build . --parallel $Parallel --config $config --target install
    if($LASTEXITCODE -ne 0) {
        throw "Build failed ($LASTEXITCODE)"
    }

    Pop-Location
}

function Run-UnitTest() {
    param(
        [Parameter(Mandatory=$true, Position = 0)][string]$directory,
        [Parameter(Mandatory=$true, Position = 1)][string]$architecture
    )

    Write-Host "Testing testdir:$directory, arch:$architecture"
    New-Item -ItemType Directory -ErrorAction Ignore C:\tmp
    New-Item -ItemType Directory -ErrorAction Ignore $directory\C\tests\data
    Push-Location $directory\C\tests\data
    if(-Not (Test-Path $directory\C\tests\data\geoblocks.json)) {
        Copy-Item $env:WORKSPACE\couchbase-lite-core\C\tests\data\geoblocks.json $directory\C\tests\data\geoblocks.json
    }

    if(-Not (Test-Path $directory\C\tests\data\names_300000.json)) {
        Copy-Item $env:WORKSPACE\couchbase-lite-core\C\tests\data\names_300000.json $directory\C\tests\data\names_300000.json
    }

    Pop-Location
    Push-Location $directory\LiteCore\tests\MinSizeRel
    & .\CppTests -r list

    $env:LiteCoreTestsQuiet=0
    if($LASTEXITCODE -ne 0) {
        throw "CppTests failed"
    }

    Pop-Location
    Push-Location $directory\C\tests\MinSizeRel
    & .\C4Tests -r list
    $env:LiteCoreTestsQuiet=0
    if($LASTEXITCODE -ne 0) {
        throw "C4Tests failed"
    }

    Pop-Location
}

$ArtifactsShaDir = "$env:WORKSPACE\artifacts\couchbase-lite-core\sha\$($ShaVersion.substring(0, 2))\$ShaVersion"
$ArtifactsBuildDir = "$env:WORKSPACE\artifacts\couchbase-lite-core\$Version\$BldNum"
New-Item -Type Directory -Path $ArtifactsShaDir -ErrorAction Ignore
New-Item -Type Directory -Path $ArtifactsBuildDir -ErrorAction Ignore
Write-Host "Building $Architectures using $Parallel parallel subjobs"

foreach ($arch in $Architectures) {
    $Target = "${arch}_Debug"
    $arch_lower = $arch.ToLowerInvariant()
    if($arch -ne "ARM") {
        Build "${env:WORKSPACE}\build_${Target}" $arch "Debug"
        Make-Package "${env:WORKSPACE}\build_${Target}\install" "couchbase-lite-core-$Version-$ShaVersion-windows-$arch_lower-debug.zip" "$arch" "DEBUG"
        Copy-Item "${env:WORKSPACE}\couchbase-lite-core-$Version-$ShaVersion-windows-$arch_lower-debug.zip" "$ArtifactsShaDir\couchbase-lite-core-windows-$arch_lower-debug.zip"
        Copy-Item "${env:WORKSPACE}\couchbase-lite-core-$Version-$ShaVersion-windows-$arch_lower-debug.zip" "$ArtifactsBuildDir\couchbase-lite-core-$Edition-$Version-$BldNum-windows-$arch_lower-debug.zip"
    }

    $Target = "${arch}_MinSizeRel"
    if($arch -ne "ARM") {
        Build "${env:WORKSPACE}\build_${Target}" $arch "MinSizeRel"
        if($Edition -eq "enterprise" -and $arch -eq "Win64") {
            Run-UnitTest "${env:WORKSPACE}\build_${Target}\couchbase-lite-core" $arch
        }

        Make-Package "${env:WORKSPACE}\build_${Target}\install" "couchbase-lite-core-$Version-$ShaVersion-windows-$arch_lower.zip" "$arch" "RELEASE"
        Copy-Item "${env:WORKSPACE}\couchbase-lite-core-$Version-$ShaVersion-windows-$arch_lower.zip" "$ArtifactsShaDir\couchbase-lite-core-windows-$arch_lower.zip"
        Copy-Item "${env:WORKSPACE}\couchbase-lite-core-$Version-$ShaVersion-windows-$arch_lower.zip" "$ArtifactsBuildDir\couchbase-lite-core-$Edition-$Version-$BldNum-windows-$arch_lower.zip"
    }
}
