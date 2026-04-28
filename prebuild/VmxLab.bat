@echo off
:: VmxLab.bat install|start|stop|uninstall|status
:: Must run as Administrator

set SVC=VmxLab
set SYS=%~dp0VmxLab.sys

if /i "%1"=="install"   goto :install
if /i "%1"=="start"     goto :start
if /i "%1"=="stop"      sc stop %SVC%   & exit /b
if /i "%1"=="uninstall" goto :uninstall
if /i "%1"=="status"    sc query %SVC%  & exit /b
echo Usage: VmxLab.bat [install^|start^|stop^|uninstall^|status]
exit /b 1

:install
sc stop   %SVC% >nul 2>&1
sc delete %SVC% >nul 2>&1
sc create %SVC% type= kernel binPath= "%SYS%"
exit /b

:start
sc start %SVC%
echo Check DebugView (Kernel Capture) for [VMX] output.
exit /b

:uninstall
sc stop   %SVC% >nul 2>&1
sc delete %SVC%
exit /b
