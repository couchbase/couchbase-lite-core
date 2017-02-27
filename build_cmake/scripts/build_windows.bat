mkdir x86
mkdir x64
cd x86
"C:\Program Files\CMake\bin\cmake.exe" ..\..
msbuild LiteCore.sln /p:Configuration=RelWithDebInfo

cd ..
cd x64
"C:\Program Files\CMake\bin\cmake.exe" -G "Visual Studio 14 2015 Win64" ..\..
msbuild LiteCore.sln /p:Configuration=RelWithDebInfo