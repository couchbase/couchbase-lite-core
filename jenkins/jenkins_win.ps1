$ErrorActionPreference = "Stop";

if($env:CHANGE_TARGET) {
    $env:BRANCH = $env:CHANGE_TARGET
}

if(! $env:WORKSPACE) {
    $env:WORKSPACE = "$PSScriptRoot\.."
}

# Jenkins is a pain because it doesn't give the option for no source
# and junctions make Git blow up so the easiest way is to just clone
# the whole freaking thing again
$COMMIT_SHA=$(& 'C:\Program Files\Git\bin\git.exe' rev-parse HEAD)
if(Test-Path $env:WORKSPACE\couchbase-lite-core) {
    Push-Location $env:WORKSPACE\couchbase-lite-core
    & 'C:\Program Files\Git\bin\git.exe' fetch origin
    & 'C:\Program Files\Git\bin\git.exe' reset --hard
    & 'C:\Program Files\Git\bin\git.exe' checkout $COMMIT_SHA
    & 'C:\Program Files\Git\bin\git.exe' clean -dfx .
    Pop-Location
} else {
    & 'C:\Program Files\Git\bin\git.exe' clone ssh://git@github.com/couchbase/couchbase-lite-core $env:WORKSPACE\couchbase-lite-core
    Push-Location $env:WORKSPACE\couchbase-lite-core
    & 'C:\Program Files\Git\bin\git.exe' checkout $COMMIT_SHA
    & 'C:\Program Files\Git\bin\git.exe' submodule update --init --recursive
    Pop-Location
}

if(Test-Path $env:WORKSPACE\couchbase-lite-core-EE) {
    Push-Location $env:WORKSPACE\couchbase-lite-core-EE
    & 'C:\Program Files\Git\bin\git.exe' fetch origin
    & 'C:\Program Files\Git\bin\git.exe' reset --hard
    & 'C:\Program Files\Git\bin\git.exe' checkout $env:BRANCH
    & 'C:\Program Files\Git\bin\git.exe' clean -dfx .
    & 'C:\Program Files\Git\bin\git.exe' pull origin $env:BRANCH
    Pop-Location
} else {
    & 'C:\Program Files\Git\bin\git.exe' clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $env:BRANCH --recursive $env:WORKSPACE\couchbase-lite-core-EE
}

New-Item -Type Directory -ErrorAction Ignore $env:WORKSPACE\couchbase-lite-core\build_cmake\x64
Push-Location $env:WORKSPACE\couchbase-lite-core\build_cmake\x64
& 'C:\Program Files\CMake\bin\cmake.exe' -G "Visual Studio 14 2015 Win64" -DBUILD_ENTERPRISE=ON ..\..
if($LASTEXITCODE -ne 0) {
    Write-Error "Failed to run CMake!"
    throw "Failed to run CMake!"
}

& 'C:\Program Files\CMake\bin\cmake.exe' --build .
if($LASTEXITCODE -ne 0) {
    Write-Error "Failed to build!"
    throw "Failed to build!"
}

$env:LiteCoreTestsQuiet=1
Push-Location LiteCore\tests\Debug
.\CppTests -r list
if($LASTEXITCODE -ne 0) {
    Write-Error "C++ tests failed!"
    throw "C++ tests failed"
}

Pop-Location
Push-Location C\tests\Debug
.\C4Tests -r list
if($LASTEXITCODE -ne 0) {
    Write-Error "C tests failed!"
    throw "C tests failed"
}

Pop-Location
Pop-Location