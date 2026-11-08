@echo off
chcp 65001 >nul
echo ========================================================
echo  OpenSmoothScroll 一键编译脚本 (MinGW-w64 GCC)
echo ========================================================

where g++ >nul 2>nul
if %errorlevel% neq 0 (
    echo [错误] 未在系统 PATH 中找到 g++ 编译器！
    echo 请安装 MinGW-w64 并将其 bin 目录加入环境变量。
    pause
    exit /b 1
)

echo [1/2] 正在编译 OpenSmoothScroll.exe...
g++ -O2 -s -mwindows src\main.cpp -o OpenSmoothScroll.exe -lwinmm -lgdi32 -lshell32

if %errorlevel% equ 0 (
    echo [2/2] 编译成功！
    echo 生成产物: OpenSmoothScroll.exe
    for %%I in (OpenSmoothScroll.exe) do echo 文件大小: %%~zI 字节 (约 69 KB)
) else (
    echo [错误] 编译失败，请检查编译输出。
)

echo ========================================================
pause
