
$startdir = [IO.Path]::Combine([string]$PSScriptRoot, '..', 'vendor', 'fleece', 'Fleece')
pushd $startdir

Copy-Item ..\..\..\C\include\parse_API.py .
Copy-Item ..\..\..\C\include\parse_structs.py .
Copy-Item ..\..\..\C\include\parse_enums.py .
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
