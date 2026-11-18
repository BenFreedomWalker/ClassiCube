@echo off
setlocal

rem ClassiCube Windows build helper for MSYS2 UCRT64.
rem Run from the repository root, either by double-clicking or from cmd.exe.

if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
if not exist "%MSYS2_ROOT%\usr\bin\bash.exe" (
    echo ERROR: MSYS2 was not found at "%MSYS2_ROOT%".
    echo Set MSYS2_ROOT to your MSYS2 installation directory.
    exit /b 1
)
if not exist "%MSYS2_ROOT%\ucrt64\bin\gcc.exe" (
    echo ERROR: The MSYS2 UCRT64 GCC toolchain is missing.
    echo Install it from the UCRT64 shell with:
    echo   pacman -S --needed mingw-w64-ucrt-x86_64-gcc make
    exit /b 1
)
if not exist "%MSYS2_ROOT%\usr\bin\make.exe" (
    echo ERROR: GNU make is missing from MSYS2.
    echo Install it from the UCRT64 shell with:
    echo   pacman -S --needed make
    exit /b 1
)

set "MSYSTEM=UCRT64"
set "MSYS=1"
set "CHERE_INVOKING=1"
set "PATH=%MSYS2_ROOT%\ucrt64\bin;%MSYS2_ROOT%\usr\bin;%PATH%"

"%MSYS2_ROOT%\ucrt64\bin\gcc.exe" -dumpmachine | "%MSYS2_ROOT%\usr\bin\grep.exe" -q "w64-mingw32"
if errorlevel 1 (
    echo ERROR: UCRT64 GCC does not report a Windows MinGW target.
    exit /b 1
)

if /I "%~1"=="clean" goto clean
if /I "%~1"=="debug" goto debug
if not "%~1"=="" (
    echo Usage: build.bat [release^|debug^|clean]
    exit /b 2
)

echo Building optimized Windows executable...
"%MSYS2_ROOT%\usr\bin\bash.exe" -c "make windows RELEASE=1"
if errorlevel 1 goto failed
goto success

:debug
echo Building debug Windows executable...
"%MSYS2_ROOT%\usr\bin\bash.exe" -c "make windows"
if errorlevel 1 goto failed
goto success

:clean
echo Cleaning Windows build outputs...
"%MSYS2_ROOT%\usr\bin\bash.exe" -c "make windows clean"
if errorlevel 1 goto failed
echo Clean complete.
exit /b 0

:success
if not exist "ClassiCube.exe" (
    echo ERROR: Build command succeeded but ClassiCube.exe was not found.
    exit /b 1
)
echo.
echo Build complete: %CD%\ClassiCube.exe
exit /b 0

:failed
echo.
echo ERROR: ClassiCube build failed. See the compiler output above.
exit /b 1
