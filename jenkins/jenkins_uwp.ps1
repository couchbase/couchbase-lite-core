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

    New-Item -Type Directory -ErrorAction Ignore couchbase-lite-core\build_cmake\x86_store
    Set-Location couchbase-lite-core\build_cmake\x86_store
    & 'C:\Program Files\CMake\bin\cmake.exe' -G "Visual Studio 15 2017" -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION="10.0" -DCMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION="10.0.16299.0" -DBUILD_ENTERPRISE=ON ..\..
    if($LASTEXITCODE -ne 0) {
        Write-Host "Failed to run CMake!" -ForegroundColor Red
        exit 1
    }

    & 'C:\Program Files\CMake\bin\cmake.exe' --build . --target LiteCore --config MinSizeRel
    if($LASTEXITCODE -ne 0) {
        Write-Host "Failed to build!" -ForegroundColor Red
        exit 1
    }
} finally {
    Pop-Location
}