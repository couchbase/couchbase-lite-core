Push-Location "$PSScriptRoot\.."
try {
    & 'C:\Program Files\Git\bin\git.exe' submodule update --init --recursive
    New-Item -Type Directory "couchbase-lite-core"
    Get-ChildItem -Path $pwd -Exclude "couchbase-lite-core" | Move-Item -Destination "couchbase-lite-core"

    # Sometimes a PR depends on a PR in the EE repo as well.  This needs to be convention based, so if there is a branch with the same name
    # as the one in this PR in the EE repo then use that, otherwise use the name of the target branch (master, release/XXX etc)
    & 'C:\Program Files\Git\bin\git.exe' clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $env:BRANCH_NAME --recursive --depth 1 couchbase-lite-core-EE
    if($LASTEXITCODE -ne 0) {
        & 'C:\Program Files\Git\bin\git.exe' clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $env:CHANGE_TARGET --recursive --depth 1 couchbase-lite-core-EE
    }

    New-Item -Type Directory -ErrorAction Ignore couchbase-lite-core\build_cmake\x64
    Set-Location couchbase-lite-core\build_cmake\x64
    & 'C:\Program Files\CMake\bin\cmake.exe' -G "Visual Studio 15 2017 Win64" -DBUILD_ENTERPRISE=ON ..\..
    if($LASTEXITCODE -ne 0) {
        Write-Host "Failed to run CMake!" -ForegroundColor Red
        exit 1
    }

    & 'C:\Program Files\CMake\bin\cmake.exe' --build .
    if($LASTEXITCODE -ne 0) {
        Write-Host "Failed to build!" -ForegroundColor Red
        exit 1
    }

    $env:LiteCoreTestsQuiet=1
    Set-Location LiteCore\tests\Debug
    .\CppTests -r list
    if($LASTEXITCODE -ne 0) {
        Write-Host "C++ tests failed!" -ForegroundColor Red
        exit 1
    }

    Set-Location ..\..\..\C\tests\Debug
    .\C4Tests -r list
    if($LASTEXITCODE -ne 0) {
        Write-Host "C tests failed!" -ForegroundColor Red
        exit 1
    }
} finally {
    Pop-Location
}