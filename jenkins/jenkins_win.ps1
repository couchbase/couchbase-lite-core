if($env:CHANGE_TARGET) {
    $env:BRANCH = $env:CHANGE_TARGET
}

Push-Location "$PSScriptRoot\.."
try {
    & 'C:\Program Files\Git\bin\git.exe' submodule update --init --recursive
    New-Item -Type Directory "couchbase-lite-core"
    Get-ChildItem -Path $pwd -Exclude "couchbase-lite-core" | Move-Item -Destination "couchbase-lite-core"
    & 'C:\Program Files\Git\bin\git.exe' clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $env:BRANCH --recursive --depth 1 couchbase-lite-core-EE

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