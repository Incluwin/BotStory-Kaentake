@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0"
cl /nologo /EHsc /std:c++17 /W4 /I..\src featureswitch_test.cpp /Fe:featureswitch_test.exe /Fo:featureswitch_test.obj
if errorlevel 1 exit /b 1
.\featureswitch_test.exe
