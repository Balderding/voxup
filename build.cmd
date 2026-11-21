@echo off
rem build.cmd: builds voxup.exe here with clang (LLVM, MinGW target). Close a running voxup.exe first.
rem clang must be on the PATH, or set LLVM_ENV to a .cmd that puts it there (and sets CLANG_FLAGS if needed).
setlocal
if defined LLVM_ENV call "%LLVM_ENV%"
cd /d "%~dp0src"
if not exist ..\obj mkdir ..\obj
rem the third-party code once, without our warnings (it only changes with third_party\VERSIONS.txt)
if not exist ..\obj\nk_impl.o clang %CLANG_FLAGS% -O2 -w -c -o ..\obj\nk_impl.o nk_impl.c || exit /b 1
clang %CLANG_FLAGS% -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-function -mwindows -static -o ..\voxup.exe pick.c shrink.c voxup.c vlog.c ..\obj\nk_impl.o -lwinhttp -lgdi32 -luser32 -lmsimg32 -lkernel32 -lm || exit /b 1
echo built voxup.exe
