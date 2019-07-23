if(Test-Path couchbase-lite-core) {
    Push-Location couchbase-lite-core
    & 'C:\Program Files\Git\bin\git.exe' fetch origin
    & 'C:\Program Files\Git\bin\git.exe' reset --hard
    & 'C:\Program Files\Git\bin\git.exe' checkout $env:BRANCH
    & 'C:\Program Files\Git\bin\git.exe' clean -dfx .
    & 'C:\Program Files\Git\bin\git.exe' pull origin $env:BRANCH
    Pop-Location
} else {
    & 'C:\Program Files\Git\bin\git.exe' clone ssh://git@github.com/couchbase/couchbase-lite-core --branch $env:BRANCH --recursive
}

if(Test-Path couchbase-lite-core-EE) {
    Push-Location couchbase-lite-core-EE
    & 'C:\Program Files\Git\bin\git.exe' fetch origin
    & 'C:\Program Files\Git\bin\git.exe' reset --hard
    & 'C:\Program Files\Git\bin\git.exe' checkout $env:BRANCH
    & 'C:\Program Files\Git\bin\git.exe' clean -dfx .
    & 'C:\Program Files\Git\bin\git.exe' pull origin $env:BRANCH
    Pop-Location
} else {
    & 'C:\Program Files\Git\bin\git.exe' clone ssh://git@github.com/couchbase/couchbase-lite-core-EE --branch $env:BRANCH --recursive
}

New-Item -Type Directory couchbase-lite-core/build_cmake/x64
Push-Location couchbase-lite-core/build_cmake/x64
& 'C:\Program Files\CMake\bin\cmake.exe' -G "Visual Studio 14 2015 Win64" -DBUILD_ENTERPRISE=ON ..\..
if($LASTEXITCODE -ne 0) {
    Po
    exit $LASTEXITCODE
}

& 'C:\Program Files\CMake\bin\cmake.exe' --build .
if($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$env:LiteCoreTestsQuiet=1
Push-Location LiteCore\tests\Debug
.\CppTests -r list
if($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Pop-Location
Push-Location C\tests\Debug
.\C4Tests -r list
if($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}