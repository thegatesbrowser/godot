@echo off
rem Build tg-wfp-tool.exe with clang-cl (preferred, matches the launcher's compiler)
rem or fall back to MSVC's cl.exe.

setlocal
set OUT=tg-wfp-tool.exe
set SRC=%~dp0main.cpp
set LLVM_BIN=C:\Program Files\LLVM\bin

if exist "%LLVM_BIN%\clang-cl.exe" (
    "%LLVM_BIN%\clang-cl.exe" /std:c++17 /EHsc /O2 "%SRC%" /link fwpuclnt.lib rpcrt4.lib advapi32.lib userenv.lib /OUT:"%~dp0%OUT%"
) else (
    where cl.exe >nul 2>&1
    if errorlevel 1 (
        echo ERROR: neither clang-cl nor cl.exe in PATH. Install LLVM or run from a VS Developer Prompt.
        exit /b 1
    )
    cl.exe /std:c++17 /EHsc /O2 "%SRC%" /link fwpuclnt.lib rpcrt4.lib advapi32.lib userenv.lib /OUT:"%~dp0%OUT%"
)
