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
    & 'C:\Program Files\CMake\bin\cmake.exe' -G "Visual Studio 15 2017" -A x64 -DBUILD_ENTERPRISE=ON ..\..
    if($LASTEXITCODE -ne 0) {
        Write-Host "Failed to run CMake!" -ForegroundColor Red
        exit 1
    }

    & 'C:\Program Files\CMake\bin\cmake.exe' --build . --parallel 8
    if($LASTEXITCODE -ne 0) {
        Write-Host "Failed to build!" -ForegroundColor Red
        exit 1
    }

    Set-Location LiteCore\tests\Debug
    .\CppTests -r quiet
    if($LASTEXITCODE -ne 0) {
        Write-Host "C++ tests failed!" -ForegroundColor Red
        exit 1
    }

    Set-Location ..\..\..\C\tests\Debug
    .\C4Tests -r quiet
    if($LASTEXITCODE -ne 0) {
        Write-Host "C tests failed!" -ForegroundColor Red
        exit 1
    }
} finally {
    Pop-Location
}