@echo off
rem MiniTodo 编译脚本 —— 需要 MinGW-w64 (g++) 在 PATH 中
rem 便携工具链位置: %LOCALAPPDATA%\lib\w64devkit\w64devkit\bin
setlocal
set PATH=%PROFILESFOLDER%\.local\lib\w64devkit\w64devkit\bin;%PATH%
windres -i "%~dp0todo.rc" -o "%~dp0todo.res" -O coff
if errorlevel 1 (echo 资源编译失败 & exit /b 1)
rem -s: 去符号表  -ffunction-sections/--gc-sections: 裁剪未引用代码段, 减小 exe 体积
g++ -O2 -municode -mwindows -static -s -ffunction-sections -Wl,--gc-sections -o "%~dp0MiniTodo.exe" "%~dp0todo.cpp" "%~dp0todo.res" -lgdi32 -lshell32 -ladvapi32 -ldwmapi -lwinhttp
if errorlevel 1 (echo 编译失败 & exit /b 1)
echo 编译成功: %~dp0MiniTodo.exe
