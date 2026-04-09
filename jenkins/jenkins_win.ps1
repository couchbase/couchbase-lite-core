# Copyright 2019-Present Couchbase, Inc.
#
# Use of this software is governed by the Business Source License included in
# the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
# file, in accordance with the Business Source License, use of this software
# will be governed by the Apache License, Version 2.0, included in the file
# licenses/APL2.txt.

Push-Location "$PSScriptRoot\.."
try {
    & 'C:\Program Files\Git\bin\git.exe' submodule update --init --recursive
    New-Item -Type Directory "couchbase-lite-core"
    Get-ChildItem -Path $pwd -Exclude "couchbase-lite-core" | Move-Item -Destination "couchbase-lite-core"

    # Sometimes a PR depends on a PR in the EE repo as well.  This needs to be convention based, so if there is a branch with the name PR-###
    # (with the GH PR number) in the EE repo then use that, otherwise use the name of the target branch (master, release/XXX etc)  
    & 'C:\Program Files\Git\bin\git.exe' clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $env:BRANCH_NAME --recursive --depth 1 couchbase-lite-core-EE
    if($LASTEXITCODE -ne 0) {
        & 'C:\Program Files\Git\bin\git.exe' clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $env:CHANGE_TARGET --recursive --depth 1 couchbase-lite-core-EE
    }

    New-Item -Type Directory -ErrorAction Ignore couchbase-lite-core\build_cmake\x64
    Set-Location couchbase-lite-core\build_cmake\x64

    # This -T version controls which version of the MSVC toolchain is used.
    # Once it is decided for a given minor release line, it should not be changed.
    & 'C:\Program Files\CMake\bin\cmake.exe' -T version=14.36.17.6 -DCMAKE_SYSTEM_VERSION="10.0" -DBUILD_ENTERPRISE=ON ..\..
    if($LASTEXITCODE -ne 0) {
        Write-Host "Failed to run CMake!" -ForegroundColor Red
        exit 1
    }

    & 'C:\Program Files\CMake\bin\cmake.exe' --build . --parallel $env:NUMBER_OF_PROCESSORS
    if($LASTEXITCODE -ne 0) {
        Write-Host "Failed to build!" -ForegroundColor Red
        exit 1
    }

    # TEMP: Using `list` not `quiet` due to Catch2 deadlock
    Set-Location LiteCore\tests\Debug
    .\CppTests -r list
    if($LASTEXITCODE -ne 0) {
        Write-Host "C++ tests failed (exit code: $LASTEXITCODE)!" -ForegroundColor Red
        exit 1
    }

    # TEMP: Using `list` not `quiet` due to Catch2 deadlock
    Set-Location ..\..\..\C\tests\Debug
    .\C4Tests -r list
    if($LASTEXITCODE -ne 0) {
        Write-Host "C tests failed (exit code: $LASTEXITCODE)!" -ForegroundColor Red
        exit 1
    }
} finally {
    Pop-Location
}
