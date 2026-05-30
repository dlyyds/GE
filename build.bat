@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

echo ==============================
echo 开始构建 GameEngine
echo ==============================


:: 2. CMake 生成
echo CMake 生成工程...
cmake -B bin-int -G "MinGW Makefiles"

:: 3. 编译
echo 开始编译...
cmake --build bin-int


echo.
echo ==============================
echo 构建完成！
echo ==============================
pause