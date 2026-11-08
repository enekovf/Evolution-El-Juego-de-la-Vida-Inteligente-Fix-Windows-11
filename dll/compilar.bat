@echo off
rem Compila SH33W32.DLL (x86) y la deja en ..\bin.
rem Requiere Visual Studio (o Build Tools) con "Desarrollo para el escritorio con C++".
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo No se encuentra Visual Studio. & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo Falta el componente de C++ de Visual Studio. & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x64_x86 >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /c /O2 /W3 /GS- /Zl sh33w32.c || exit /b 1
link /nologo /DLL /NODEFAULTLIB /ENTRY:DllMain /DEF:sh33w32.def /OUT:SH33W32.DLL /SUBSYSTEM:WINDOWS,5.01 sh33w32.obj kernel32.lib user32.lib gdi32.lib advapi32.lib ole32.lib || exit /b 1
if not exist ..\bin mkdir ..\bin
copy /y SH33W32.DLL ..\bin\SH33W32.DLL >nul
del sh33w32.obj SH33W32.lib SH33W32.exp SH33W32.DLL 2>nul
echo Compilada: bin\SH33W32.DLL
