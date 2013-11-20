@echo off
rem Update the version resource in the executable from version.h

path c:\bin;%PATH%

set EXE=%1
if "%EXE%"=="" set EXE=*.exe

set FILE=version.h

set MAJOR=1
set MINOR=0
set REV=0
set BUILD=1

rem Update the build number in version.h

for /F "tokens=1,2,3" %%A in (%FILE%) do set %%B=%%C

set /A BUILD=%BUILD%+1

echo #define MAJOR %MAJOR% > %FILE%
echo #define MINOR %MINOR% >> %FILE%
echo #define REV %REV% >> %FILE%
echo #define BUILD %BUILD% >> %FILE%


rem Update the rev number from source control.
rem If source control not avail, use REV from version.h as is.

subwcrev . %FILE% %FILE%.tmp > NUL
if exist %FILE%.tmp for /F "tokens=1,2,3" %%A in (%FILE%.tmp) do set %%B=%%C
if exist %FILE%.tmp del %FILE%.tmp

if %REV% GEQ 0 ( set a=b ) else set REV=0

rem Patch the executable version resource.

set VER=%MAJOR%.%MINOR%.%REV%.%BUILD%
echo version %VER%
for %%F in (%EXE%) do (
	echo Patching %%F
	verpatch %%F "%VER% " /pv "%VER% " > NUL
)

