@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem Builds CoD Xe (Release, Xbox 360) with Visual Studio 2010 and the Xbox 360 SDK.
rem
rem First it fixes what keeps Visual Studio 2010 from opening codxe.vcxproj on Windows 10 and 11
rem ("Project Target Framework Not Installed", .NETFramework,Version=v4.0): Windows ships .NET 4.8,
rem which runs .NET 4.0 programs, but the registry entries Visual Studio 2010 looks for to decide that
rem .NET Framework 4 is installed can be missing, and the .NET 4 installer then refuses to run.
rem
rem   build-codxe.bat            fix if needed, then build
rem   build-codxe.bat /fixonly   only fix, then open codxe.sln in Visual Studio 2010

cd /d "%~dp0.."
set "SELF=%~f0"
set "MODE=%~1"
set "SKU_KEY=HKLM\SOFTWARE\Microsoft\.NETFramework\v4.0.30319\SKUs"
set "MSBUILD=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe"
set "PF86=%ProgramFiles(x86)%"
if "%PF86%"=="" set "PF86=%ProgramFiles%"
set "REFASM=%PF86%\Reference Assemblies\Microsoft\Framework\.NETFramework\v4.0"

echo === CoD Xe build ===
echo.

rem --- what is installed ---
set "PROBLEMS=0"
if not defined VS100COMNTOOLS (
    echo [missing] Visual Studio 2010: VS100COMNTOOLS is not set. Install Visual Studio 2010 Ultimate, then SP1.
    set "PROBLEMS=1"
) else (
    echo [ok] Visual Studio 2010: !VS100COMNTOOLS!
)
if not defined XEDK (
    echo [missing] Xbox 360 SDK: XEDK is not set. Install it after Visual Studio 2010, with its Visual Studio integration.
    set "PROBLEMS=1"
) else (
    echo [ok] Xbox 360 SDK: !XEDK!
)
if not exist "%MSBUILD%" (
    echo [missing] MSBuild 4.0: !MSBUILD!
    set "PROBLEMS=1"
) else (
    echo [ok] MSBuild 4.0
)
if "%PROBLEMS%"=="1" (
    echo.
    echo Install what is missing and run this again.
    goto :end
)

rem --- the .NET Framework 4 registration Visual Studio 2010 looks for ---
set "NEED_FIX=0"
for %%V in (32 64) do (
    reg query "%SKU_KEY%\.NETFramework,Version=v4.0" /reg:%%V >nul 2>&1 || set "NEED_FIX=1"
    reg query "%SKU_KEY%\.NETFramework,Version=v4.0,Profile=Client" /reg:%%V >nul 2>&1 || set "NEED_FIX=1"
)
if "%NEED_FIX%"=="0" goto :registered
net session >nul 2>&1
if not errorlevel 1 goto :register
echo.
echo Visual Studio 2010 does not see .NET Framework 4. Fixing that needs administrator rights:
echo continuing in a new window, as administrator.
set "ELEVATE=Start-Process -FilePath '%SELF%' -Verb RunAs"
if defined MODE set "ELEVATE=%ELEVATE% -ArgumentList '%MODE%'"
powershell -NoProfile -Command "%ELEVATE%"
goto :eof

:register
for %%V in (32 64) do (
    reg add "%SKU_KEY%\.NETFramework,Version=v4.0" /ve /d ".NET Framework 4" /f /reg:%%V >nul
    reg add "%SKU_KEY%\.NETFramework,Version=v4.0,Profile=Client" /ve /d ".NET Framework 4 Client Profile" /f /reg:%%V >nul
)
echo [fixed] .NET Framework 4 is registered for Visual Studio 2010
goto :reference_assemblies

:registered
echo [ok] .NET Framework 4 is registered for Visual Studio 2010

rem --- the .NET 4 reference assemblies, part of Visual Studio 2010 (its multi-targeting pack) ---
:reference_assemblies
if exist "%REFASM%\mscorlib.dll" (
    echo [ok] .NET Framework 4 reference assemblies
    goto :built_or_fixed
)
echo [missing] .NET Framework 4 reference assemblies in %REFASM%
set "MTPACK="
for %%D in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if not defined MTPACK if exist "%%D:\WCU\MTPack\netfx_core_mtpack.msi" set "MTPACK=%%D:\WCU\MTPack"
)
if not defined MTPACK (
    echo Mount the Visual Studio 2010 disc image and run this again to install them from WCU\MTPack,
    echo or repair Visual Studio 2010 from Programs and Features. The build may work without them.
    goto :built_or_fixed
)
echo Installing them from the Visual Studio 2010 disc, %MTPACK%...
msiexec /i "%MTPACK%\netfx_core_mtpack.msi" /passive /norestart
if exist "%MTPACK%\netfx_extended_mtpack.msi" msiexec /i "%MTPACK%\netfx_extended_mtpack.msi" /passive /norestart

:built_or_fixed
if /i not "%MODE%"=="/fixonly" goto :build
echo.
echo Done. Restart Visual Studio 2010, open codxe.sln, right click "codxe (unavailable)" and pick Reload Project.
goto :end

rem --- build ---
:build
echo.
echo Building the Release configuration for Xbox 360...
"%MSBUILD%" codxe.sln /m /nologo /v:minimal "/p:Configuration=Release" "/p:Platform=Xbox 360"
if not errorlevel 1 goto :success
echo.
echo The build failed, see above. If it says the 2010-01 platform toolset or the Xbox 360 platform is
echo missing, reinstall the Xbox 360 SDK with its Visual Studio 2010 integration, after Visual Studio 2010 SP1.
goto :end

:success
echo.
echo Built: %CD%\build\Release\bin\codxe.xex
echo Copy it over codxe.xex in the plugins\4156081C folder of Xenia, keeping the old one as a backup.

:end
echo.
pause
