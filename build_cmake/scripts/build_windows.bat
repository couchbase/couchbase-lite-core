pushd %~dp0\..
mkdir x86
mkdir x64
mkdir arm
cd x86
"C:\Program Files\CMake\bin\cmake.exe" -D-DBLIP_NO_FRAMING=ON ..\..
msbuild LiteCore.sln /p:Configuration=RelWithDebInfo

cd ..
cd x64
"C:\Program Files\CMake\bin\cmake.exe" -G "Visual Studio 14 2015 Win64" .-DBLIP_NO_FRAMING=ON .\..
msbuild LiteCore.sln /p:Configuration=RelWithDebInfo
popd
