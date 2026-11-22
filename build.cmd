@echo off
rem build.cmd: builds voxup.exe here with clang (LLVM, MinGW target). Close a running voxup.exe first.
rem clang must be on the PATH, or set LLVM_ENV to a .cmd that puts it there (and sets CLANG_FLAGS if needed).
rem
rem   build.cmd       voxup with ComfyUI only
rem   build.cmd sd    voxup with the built-in engine too (stable-diffusion.cpp, Vulkan). Set SDCPP to its source folder
rem                   and SDCPP_BUILD to its CMake build folder (built with -DSD_VULKAN=ON -DGGML_OPENMP=OFF), and
rem                   VULKAN_SDK to the Vulkan SDK. See README.md, "The built-in engine".
setlocal
if defined LLVM_ENV call "%LLVM_ENV%"
cd /d "%~dp0src"
if not exist ..\obj mkdir ..\obj
set W=-O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-function
rem the third-party code once, without our warnings (it only changes with third_party\VERSIONS.txt)
if not exist ..\obj\nk_impl.o clang %CLANG_FLAGS% -O2 -w -c -o ..\obj\nk_impl.o nk_impl.c || exit /b 1
if /i not "%1"=="sd" (
  clang %CLANG_FLAGS% %W% -mwindows -static -o ..\voxup.exe pick.c shrink.c upright.c voxup.c vlog.c ..\obj\nk_impl.o -lwinhttp -lgdi32 -luser32 -lmsimg32 -lkernel32 -lm || exit /b 1
  echo built voxup.exe
  exit /b 0
)
if not defined SDCPP (echo set SDCPP to the stable-diffusion.cpp source folder & exit /b 1)
if not defined SDCPP_BUILD (echo set SDCPP_BUILD to its build folder & exit /b 1)
if not defined VULKAN_SDK (echo set VULKAN_SDK to the Vulkan SDK folder & exit /b 1)
set B=%SDCPP_BUILD%
clang %CLANG_FLAGS% %W% -DVOXUP_SD "-I%SDCPP%\include" -c -o ..\obj\pick-sd.o pick.c || exit /b 1
clang %CLANG_FLAGS% %W% -c -o ..\obj\shrink.o shrink.c || exit /b 1
clang %CLANG_FLAGS% %W% -c -o ..\obj\voxup.o voxup.c || exit /b 1
clang %CLANG_FLAGS% %W% -c -o ..\obj\vlog.o vlog.c || exit /b 1
clang %CLANG_FLAGS% %W% -c -o ..\obj\upright.o upright.c || exit /b 1
clang++ %CLANG_FLAGS% -O2 -mwindows -static -o ..\voxup.exe ..\obj\pick-sd.o ..\obj\shrink.o ..\obj\voxup.o ..\obj\vlog.o ..\obj\upright.o ..\obj\nk_impl.o ^
  "%B%\libstable-diffusion.a" "%B%\ggml\src\ggml.a" "%B%\ggml\src\ggml-cpu.a" "%B%\ggml\src\ggml-vulkan\ggml-vulkan.a" "%B%\ggml\src\ggml-base.a" ^
  "%VULKAN_SDK%\Lib\vulkan-1.lib" -lpthread -lwinspool -lshell32 -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32 ^
  -lwinhttp -lgdi32 -luser32 -lmsimg32 -lkernel32 -lm || exit /b 1
echo built voxup.exe with the built-in engine
