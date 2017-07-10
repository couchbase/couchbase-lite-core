pushd $PSScriptRoot\..\C\include
python parse_API.py -c config
python parse_structs.py
Move-Item -Force *.template ..\..\CSharp\src\LiteCore.Shared\Interop
Move-Item -Force *.cs ..\..\CSharp\src\LiteCore.Shared\Interop
pushd ..\..\CSharp\src\LiteCore.Shared\Interop
python gen_bindings.py
rm *.template
popd
popd
