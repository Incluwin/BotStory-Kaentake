@echo off
rem Checks src/dllentry.cpp: at process exit a DLL with KaentakeDllEntry skips its static destructors
rem (one here faults, like Kaentake's did on every quit), and FreeLibrary still runs them.
rem The same DLL with the CRT's own entry point shows the fault. Exit code 0 = all checks passed.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0"
cl /nologo /c /EHsc /std:c++17 /W4 /MD dllentry_test_dll.cpp ..\src\dllentry.cpp
if errorlevel 1 exit /b 1
link /nologo /DLL /OUT:dllentry_test_fixed.dll /ENTRY:KaentakeDllEntry dllentry_test_dll.obj dllentry.obj
if errorlevel 1 exit /b 1
link /nologo /DLL /OUT:dllentry_test_stock.dll dllentry_test_dll.obj
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /W4 /MD dllentry_test.cpp /Fe:dllentry_test.exe /Fo:dllentry_test.obj
if errorlevel 1 exit /b 1
set FAIL=0
.\dllentry_test.exe exit dllentry_test_stock.dll
if not "%errorlevel%"=="-1073741819" (echo FAIL: CRT entry exit code %errorlevel%, expected the fault -1073741819 & set FAIL=1)
.\dllentry_test.exe exit dllentry_test_fixed.dll
if not "%errorlevel%"=="7" (echo FAIL: KaentakeDllEntry exit code %errorlevel%, expected 7 & set FAIL=1)
.\dllentry_test.exe unload dllentry_test_fixed.dll
if not "%errorlevel%"=="0" (echo FAIL: FreeLibrary did not run the static destructor, code %errorlevel% & set FAIL=1)
if %FAIL%==1 exit /b 1
echo dllentry_test: all checks passed
exit /b 0
