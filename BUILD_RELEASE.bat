@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

echo ========================================
echo       WireScope Native Builder
echo ========================================
echo.

echo [1/4] Finding Visual Studio C++ build tools...
set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

rem 1) Use vswhere when available.
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find MSBuild\**\Bin\MSBuild.exe 2^>nul`) do (
    if not defined MSBUILD set "MSBUILD=%%i"
  )
)

rem 2) If MSBuild is already in PATH (Developer Command Prompt, Build Tools, etc.).
if not defined MSBUILD (
  for /f "delims=" %%i in ('where MSBuild.exe 2^>nul') do (
    if not defined MSBUILD set "MSBUILD=%%i"
  )
)

rem 3) Common Visual Studio 2022 install locations. VS 2022 is 64-bit and normally lives under Program Files.
if not defined MSBUILD (
  for %%E in (Community Professional Enterprise BuildTools) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\MSBuild\Current\Bin\MSBuild.exe" (
      set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\MSBuild\Current\Bin\MSBuild.exe"
    )
  )
)

rem 4) Also check x86 root just in case a custom/older installation used it.
if not defined MSBUILD (
  for %%E in (Community Professional Enterprise BuildTools) do (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\MSBuild\Current\Bin\MSBuild.exe" (
      set "MSBUILD=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\MSBuild\Current\Bin\MSBuild.exe"
    )
  )
)

if not defined MSBUILD (
  echo.
  echo [ERROR] Visual Studio 2022 C++ build tools were not found.
  echo.
  echo You only need the compiler on YOUR PC to build WireScope.
  echo People who run the final Release do NOT need Visual Studio or .NET.
  echo.
  echo Install either:
  echo   - Visual Studio 2022 Community, or
  echo   - Build Tools for Visual Studio 2022 ^(smaller^)
  echo.
  echo In Visual Studio Installer select:
  echo   [X] Desktop development with C++
  echo And keep these components enabled:
  echo   [X] MSVC v143 - VS 2022 C++ x64/x86 build tools
  echo   [X] Windows 10 or Windows 11 SDK
  echo.
  pause
  exit /b 1
)

echo      MSBuild: %MSBUILD%

rem Verify that the v143 C++ toolset actually exists; MSBuild alone is not enough.
set "VCTOOLSFOUND="
for %%R in ("%ProgramFiles%\Microsoft Visual Studio\2022" "%ProgramFiles(x86)%\Microsoft Visual Studio\2022") do (
  if exist "%%~R" (
    for %%E in (Community Professional Enterprise BuildTools) do (
      if exist "%%~R\%%E\VC\Tools\MSVC" set "VCTOOLSFOUND=1"
    )
  )
)

if not defined VCTOOLSFOUND (
  echo.
  echo [ERROR] MSBuild was found, but the MSVC v143 C++ compiler was not found.
  echo Open Visual Studio Installer ^> Modify ^> Desktop development with C++.
  echo Make sure "MSVC v143 - VS 2022 C++ x64/x86 build tools" is checked.
  echo.
  pause
  exit /b 1
)

echo [2/4] Cleaning previous output...
if exist "bin" rmdir /s /q "bin"
if exist "obj" rmdir /s /q "obj"
if exist "Release" rmdir /s /q "Release"
mkdir "Release" >nul

echo [3/4] Building Native x64 Release...
"%MSBUILD%" "WireScopeNative.sln" /m /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:minimal
if errorlevel 1 (
  echo.
  echo [ERROR] Native build failed. Copy the first error Cxxxx or LNKxxxx line and the lines around it.
  pause
  exit /b 1
)

echo [4/4] Creating clean release folder...
for %%F in (
  "WireScope.exe"
  "WireScope.Core.dll"
  "WireScope.Network.dll"
  "WireScope.Filters.dll"
  "WireScope.Geo.dll"
) do (
  if not exist "bin\Release\%%~F" (
    echo [ERROR] Missing bin\Release\%%~F
    pause
    exit /b 1
  )
  copy /y "bin\Release\%%~F" "Release\%%~F" >nul
)

echo.
echo DONE - Native release created:
echo %CD%\Release
echo.
dir /b "Release"
echo.
echo The final Release does NOT use .NET and does NOT need Visual Studio.
echo It only expects Npcap to already be installed on the PC.
echo.
pause
