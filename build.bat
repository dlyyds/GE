@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

echo ==============================
echo GameEngine - MSVC 构建
echo ==============================

:: ==========================================
:: 路径配置
:: ==========================================
set "MSVC_TOOLS=F:\c++budiltool"
set "VCVARS_PATH=%MSVC_TOOLS%\VC\Auxiliary\Build\vcvarsall.bat"
set "NINJA_PATH=%MSVC_TOOLS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

if not exist "%VCVARS_PATH%" (
    echo [错误] 找不到 vcvarsall.bat
    echo        请修改 MSVC_TOOLS 变量为正确的 MSVC 安装路径
    pause
    exit /b 1
)

:: ==========================================
:: 默认配置
:: ==========================================
set BUILD_TYPE=Debug
set BUILD_DIR=bin-int-msvc
set CLEAN=0

:: ==========================================
:: 解析命令行参数
:: ==========================================
:parse_args
if "%~1"=="" goto :done_parsing
if /i "%~1"=="release" (
    set BUILD_TYPE=Release
) else if /i "%~1"=="debug" (
    set BUILD_TYPE=Debug
) else if /i "%~1"=="clean" (
    set CLEAN=1
) else if /i "%~1"=="rebuild" (
    set CLEAN=1
) else if /i "%~1"=="-h" (
    goto :show_help
) else if /i "%~1"=="--help" (
    goto :show_help
) else (
    echo [错误] 未知参数: %~1
    goto :show_help
)
shift
goto :parse_args
:done_parsing

:: ==========================================
:: 初始化 MSVC 编译环境
:: ==========================================
echo [信息] 初始化 MSVC 编译环境...
call "%VCVARS_PATH%" x64 >nul
if %errorlevel% neq 0 (
    echo [错误] MSVC 环境初始化失败！
    pause
    exit /b 1
)
echo [信息] MSVC 环境初始化完成

:: 将 Ninja 加入 PATH
set "PATH=%NINJA_PATH%;%PATH%"

:: ==========================================
:: 清理
:: ==========================================
if "%CLEAN%"=="1" (
    echo [信息] 清理构建目录 %BUILD_DIR%...
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo [信息] 清理完成
)

:: ==========================================
:: CMake 配置
:: ==========================================
echo.
echo [信息] CMake 配置中... (构建类型: %BUILD_TYPE%, 生成器: Ninja)
cmake -B "%BUILD_DIR%" -G "Ninja" -DCMAKE_BUILD_TYPE=%BUILD_TYPE%

if %errorlevel% neq 0 (
    echo.
    echo [错误] CMake 配置失败！
    pause
    exit /b 1
)

:: ==========================================
:: 编译
:: ==========================================
echo.
echo [信息] 开始编译...
cmake --build "%BUILD_DIR%" --config %BUILD_TYPE%

if %errorlevel% neq 0 (
    echo.
    echo [错误] 编译失败！
    pause
    exit /b 1
)

:: ==========================================
:: 完成
:: ==========================================
echo.
echo ==============================
echo 构建完成！
echo   构建类型: %BUILD_TYPE%
echo   输出目录: bin\
echo ==============================
endlocal
pause
exit /b 0

:: ==========================================
:: 帮助信息
:: ==========================================
:show_help
echo.
echo 用法: build.bat [选项]
echo.
echo 选项:
echo   debug      - Debug 构建（默认）
echo   release    - Release 构建
echo   clean      - 构建前清理
echo   rebuild    - 清理后重新构建
echo   -h, --help - 显示帮助
echo.
echo 示例:
echo   build.bat                  - Debug 构建
echo   build.bat release          - Release 构建
echo   build.bat release rebuild  - 清理后 Release 构建
echo   build.bat debug clean      - 清理后 Debug 构建
pause
exit /b 0