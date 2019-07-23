$ErrorActionPreference = "Stop";

if($env:CHANGE_TARGET) {
    $env:BRANCH = $env:CHANGE_TARGET
}

if(! $env:WORKSPACE) {
    $env:WORKSPACE = "$PSScriptRoot\.."
}

# Jenkins is a pain because it doesn't give the option for no source
# so instead just put a junction to the current directory in
if(!(Test-Path $env:WORKSPACE\couchbase-lite-core)) {
    New-Item -Type Junction -Target $env:WORKSPACE $env:WORKSPACE\couchbase-lite-core
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

New-Item -Type Directory $env:WORKSPACE\couchbase-lite-core\build_cmake\x64
Push-Location $env:WORKSPACE\couchbase-lite-core\build_cmake\x64
& 'C:\Program Files\CMake\bin\cmake.exe' -G "Visual Studio 14 2015 Win64" -DBUILD_ENTERPRISE=ON ..\..
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
Push-Location LiteCore\tests\Debug
.\CppTests -r list
if($LASTEXITCODE -ne 0) {
    Write-Host "C++ tests failed!" -ForegroundColor Red
    exit 1
}

Pop-Location
Push-Location C\tests\Debug
.\C4Tests -r list
if($LASTEXITCODE -ne 0) {
    Write-Host "C tests failed!" -ForegroundColor Red
    exit 1
}

Pop-Location
Pop-Location