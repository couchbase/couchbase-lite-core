pushd $PSScriptRoot\..\vendor\fleece\Fleece
cp ..\..\..\C\include\parse_API.py .
cp ..\..\..\C\include\parse_structs.py .
cp ..\..\..\C\include\parse_enums.py .
python parse_API.py -c config
python parse_structs.py
Move-Item -Force *.template ..\..\..\CSharp\src\LiteCore.Shared\Interop
Move-Item -Force *.cs ..\..\..\CSharp\src\LiteCore.Shared\Interop
rm parse_*.py
rm *.pyc
popd

pushd $PSScriptRoot\..\C\include
python parse_API.py -c config
python parse_structs.py
Move-Item -Force *.template ..\..\CSharp\src\LiteCore.Shared\Interop
Move-Item -Force *.cs ..\..\CSharp\src\LiteCore.Shared\Interop
rm *.pyc
pushd ..\..\CSharp\src\LiteCore.Shared\Interop
python gen_bindings.py
rm *.template
popd
popd
