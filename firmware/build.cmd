@echo off
REM Build MAKCU firmwares with the workspace-local PlatformIO + toolchain.
REM
REM   build.cmd            both
REM   build.cmd host       fw_host  -> env RIGHT
REM   build.cmd device     fw_device -> env LEFT
REM
REM See BUILD.md for why this wrapper exists instead of a plain "pio run".

setlocal
set "ROOT=%~dp0"
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

set "TOOLCHAIN=%ROOT%piocore\packages\toolchain-xtensa-esp32s3\bin"
if not exist "%TOOLCHAIN%" (
  echo Toolchain missing at %TOOLCHAIN%
  echo Run: python pioinstall.py ^&^& python _pio_setup.py ^&^& python _set_owners.py
  exit /b 1
)

REM The xtensa driver needs its own bin dir on PATH to spawn cc1plus.
set "PATH=%TOOLCHAIN%;%PATH%"
set "PYTHONPATH=%ROOT%.piopkgs"
set "PLATFORMIO_CORE_DIR=%ROOT%piocore"
set "PIO=python -c "from platformio.__main__ import main; main()""

set "FAIL=0"

if "%TARGET%"=="all"    goto :host
if "%TARGET%"=="host"   goto :host
if "%TARGET%"=="device" goto :device
echo Unknown target "%TARGET%" ^(use: all ^| host ^| device^)
exit /b 1

:host
echo === fw_host ^(env RIGHT^) ===
pushd "%ROOT%fw_host"
%PIO% run
if errorlevel 1 set "FAIL=1"
popd
if "%TARGET%"=="host" goto :done

:device
echo === fw_device ^(env LEFT^) ===
pushd "%ROOT%fw_device"
%PIO% run -e LEFT
if errorlevel 1 set "FAIL=1"
popd

:done
echo.
echo Artifacts:
if exist "%ROOT%fw_host\.pio\build\RIGHT\firmware.bin" (
  echo   fw_host   %ROOT%fw_host\.pio\build\RIGHT\firmware.bin
) else (
  echo   fw_host   MISSING
)
if exist "%ROOT%fw_device\.pio\build\LEFT\firmware.bin" (
  echo   fw_device %ROOT%fw_device\.pio\build\LEFT\firmware.bin
) else (
  echo   fw_device MISSING
)

if "%FAIL%"=="1" (
  echo.
  echo BUILD FAILED
  exit /b 1
)
echo.
echo BUILD OK
endlocal
