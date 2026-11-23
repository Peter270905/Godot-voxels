@echo off
setlocal
title Building Godot Voxel Engine (x86_64)

echo ========================================================
echo Building Godot Editor with Voxel Tools
echo ========================================================

cd /d "%~dp0godot"

echo Current directory: %CD%
echo Running SCons with %NUMBER_OF_PROCESSORS% parallel jobs...
echo.

scons platform=windows target=editor accesskit=no d3d12=no angle=no -j%NUMBER_OF_PROCESSORS%

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo BUILD SUCCESSFUL!
    echo Binary: godot\bin\godot.windows.editor.x86_64.exe
    echo ========================================================
) else (
    echo.
    echo ========================================================
    echo BUILD FAILED!
    echo Ensure Visual Studio 2022 (MSVC C++) and SCons are installed.
    echo ========================================================
)

echo.
pause
