@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

:: 防御 PATH 污染：从 Git Bash / MSYS 里调 cmd 时 /usr/bin 排在 PATH 前面，
:: 脚本里的 `find` 会被解析成 MSYS 的 GNU find —— 它会去扫整个盘，实测卡死十分钟
:: （报一堆 C:\ 下的 Permission denied）。把系统目录提到最前面，保证解析到 Windows 工具。
set "PATH=%SystemRoot%\System32;%SystemRoot%;%PATH%"

:: ============================================================
:: GameEngine - Android APK 构建
:: ============================================================
:: 把「能出一个正确的 APK」所需的全部步骤收成一条命令。默认流程：
::
::   1. gepack 收资产 → dist/            （只读资产根的来源，见 docs/Android运行与打包流程.md §3.1）
::   2. 删掉旧 APK                        （AGP 增量打包不回收缩小条目的空间，会留孤儿字节）
::   3. gradlew assembleDebug
::   4. 解包校验产物                      （构建成功 != 产物正确，必须解包看）
::
:: 为什么第 4 步要写进脚本：本项目有两次「构建照样成功、产物却是错的」前科 ——
::   · libmain.so 命名不对 → APK 里塞的是 libGE_Runtime.so，native main 永不执行
::   · Gradle 资产多套一层   → APK 里同时存在 assets/** 与 assets/assets/**
:: 两者都只有解包才看得出，且症状都极具误导性。这里用 JDK 自带的 jar 列条目来兜住。
:: ============================================================

set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"

echo ==============================
echo GameEngine - Android APK 构建
echo ==============================

:: ==========================================
:: 工具链路径
:: ==========================================
:: 环境变量为空或指向不存在的目录时回退到本机已知正确的位置。这条兜底不是多余的：
:: 本机这两个变量曾双双指向已删除的安装，而 Gradle 报的错不会指向这里。
set "JAVA_HOME_FALLBACK=E:\software\jdk\jdk17"
set "ANDROID_HOME_FALLBACK=E:\software\android-sdk"
set "USED_FALLBACK=0"

if not exist "%JAVA_HOME%\bin\java.exe" (
    set "JAVA_HOME=%JAVA_HOME_FALLBACK%"
    set "USED_FALLBACK=1"
)
if not exist "%ANDROID_HOME%\platform-tools\adb.exe" (
    set "ANDROID_HOME=%ANDROID_HOME_FALLBACK%"
    set "USED_FALLBACK=1"
)
if "%USED_FALLBACK%"=="1" (
    echo [警告] 环境变量不可用，已回退: JAVA_HOME=%JAVA_HOME%  ANDROID_HOME=%ANDROID_HOME%
)

if not exist "%JAVA_HOME%\bin\java.exe" (
    echo [错误] 找不到 JDK: %JAVA_HOME%\bin\java.exe
    goto :fail
)

set "ADB=%ANDROID_HOME%\platform-tools\adb.exe"
set "GRADLE_DIR=%REPO%\platform\android"
set "APK=%GRADLE_DIR%\app\build\outputs\apk\debug\app-debug.apk"
set "PKG=com.ge.runtime"
set "ACT=com.ge.runtime/.GEActivity"

:: ==========================================
:: 默认配置
:: ==========================================
set PACK=1
set DO_INSTALL=0
set CLEAN=0

:: ==========================================
:: 解析命令行参数
:: ==========================================
:parse_args
if "%~1"=="" goto :done_parsing
if /i "%~1"=="nopack" (
    set PACK=0
) else if /i "%~1"=="pack" (
    set PACK=1
) else if /i "%~1"=="install" (
    set DO_INSTALL=1
) else if /i "%~1"=="clean" (
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
:: 前置检查
:: ==========================================
if not exist "%GRADLE_DIR%\gradlew.bat" (
    echo [错误] 找不到 %GRADLE_DIR%\gradlew.bat
    goto :fail
)

if "%PACK%"=="1" (
    if not exist "%REPO%\bin\gepack.exe" (
        echo [错误] 找不到 bin\gepack.exe —— 先跑一次桌面构建: build.bat
        goto :fail
    )
) else (
    if not exist "%REPO%\dist\assets" (
        echo [错误] dist\assets 不存在，无法跳过资产收集 —— 去掉 nopack 参数，或先跑 build.bat
        goto :fail
    )
)

:: ==========================================
:: 1. 收集资产（gepack）
:: ==========================================
if "%PACK%"=="0" goto :skip_pack

echo.
echo [信息] 收集资产（gepack）...
:: --scene 用规范形（不带 assets/ 前缀）；--out 默认即 dist。
pushd "%REPO%"
bin\gepack.exe --scene scenes/2.scene --out dist
set "PACK_RC=%errorlevel%"
popd
if not "%PACK_RC%"=="0" (
    echo.
    echo [错误] gepack 失败（退出码 %PACK_RC%）—— 资产没变的话可以加 nopack 跳过
    goto :fail
)

:skip_pack

:: ==========================================
:: 2. 清掉旧 APK
:: ==========================================
:: **无条件**删：packageDebug 不回收"尺寸变了的条目"的旧空间，也不截断文件，
:: 会留下中央目录根本不索引的孤儿字节。触发条件不是只有"资产变小"——
:: 实测：只重编了一次 .so（改了 Renderer3D_Lifecycle.cpp），APK 就从 142MB 涨到 183MB，
:: 其中 41.3MB 孤儿，正好等于 lib/x86_64/libmain.so 的大小。即**每次改 C++ 都会中招**。
:: 代价只是重新打包一次（实测数秒），换来"产物体积可解释"。
if exist "%APK%" (
    echo [信息] 删除旧 APK（避免增量打包留下孤儿字节）
    del /q "%APK%"
)

:: ==========================================
:: 3. 构建 APK
:: ==========================================
if "%CLEAN%"=="1" (
    echo.
    echo [信息] gradlew clean（改了构建结构时才需要，较慢）...
    pushd "%GRADLE_DIR%"
    call .\gradlew.bat clean --console=plain
    set "CLEAN_RC=!errorlevel!"
    popd
    if not "!CLEAN_RC!"=="0" (
        echo [错误] gradlew clean 失败
        goto :fail
    )
)

echo.
echo [信息] gradlew assembleDebug（首次构建要编全部 .so，可能十几分钟）...
pushd "%GRADLE_DIR%"
:: 必须写 .\gradlew.bat —— 本机 cmd 不搜索当前目录找可执行文件，只写 gradlew.bat 会报
:: "is not recognized"，而那个失败在某些调用方式下退出码仍是 0，看起来像构建成功。
call .\gradlew.bat assembleDebug --console=plain
set "GRADLE_RC=%errorlevel%"
popd
if not "%GRADLE_RC%"=="0" (
    echo.
    echo [错误] gradlew assembleDebug 失败（退出码 %GRADLE_RC%）
    goto :fail
)

:: ==========================================
:: 4. 解包校验产物
:: ==========================================
:: 构建成功只说明 Gradle 没报错。以下每一条都对应一次真实踩过的坑。
echo.
echo [信息] 校验产物...

if not exist "%APK%" (
    echo [错误] 构建报告成功，但 APK 不存在：%APK%
    goto :fail
)

for %%A in ("%APK%") do set "APK_SIZE=%%~zA"
for %%A in ("%APK%") do set "APK_TIME=%%~tA"
set /a APK_MB=%APK_SIZE%/1048576
set "ASSET_COUNT=0"

set "JAR=%JAVA_HOME%\bin\jar.exe"
if not exist "%JAR%" (
    echo [警告] 找不到 jar.exe，跳过包内结构校验（只报体积）
    goto :report
)

set "ENTRIES=%TEMP%\ge_apk_entries.txt"
"%JAR%" tf "%APK%" > "%ENTRIES%" 2>nul
if not exist "%ENTRIES%" (
    echo [警告] 无法列出 APK 条目，跳过包内结构校验
    goto :report
)

:: 4a. 原生库命名：SDL 的 Java 启动器按约定找 libmain.so 并 dlsym("SDL_main")，
::     名字不对时 dlopen 失败、native main 一次都不执行，且失败是静默的。
findstr /c:"lib/arm64-v8a/libmain.so" "%ENTRIES%" >nul
if errorlevel 1 (
    echo [错误] APK 里没有 lib/arm64-v8a/libmain.so —— 检查 CMake 的 OUTPUT_NAME main
    goto :fail
)

:: 4b. 发行配置必须在 assets 根（GameConfig 在 Android 上就从这个位置读）
findstr /c:"assets/game.cfg" "%ENTRIES%" >nul
if errorlevel 1 (
    echo [错误] APK 里没有 assets/game.cfg —— 检查 copyGameAssets 的输入
    goto :fail
)

:: 4c. 最隐蔽的一条：多套一层 assets/。构建照样成功，只有解包才看得出。
findstr /b /c:"assets/assets/" "%ENTRIES%" >nul
if not errorlevel 1 (
    echo [错误] APK 里混进了 assets/assets/ 一层 —— Gradle 把资产拷到了错的位置
    echo        见 platform\android\app\build.gradle 的 copyGameAssets
    goto :fail
)

:: 4d. dist 与 APK 逐个文件对账 —— 专治"静默漏收"
::     构建成功 != 资产都在包里。实测踩到的规则：**AGP 的资源合并会忽略下划线开头的
::     目录**（当时叫 environments/_default_cube/，现已改名 DefaultCube —— 整目录被丢，而 APK 照样构建成功，只有解包
::     对账才看得出）。下划线开头的**文件**不受影响（已实测对照）。
::     注：文件名含 ! 时下面的延迟展开会被吃掉；仓库当前无此类资产名。
set "MISSING=0"
for /r "%REPO%\dist\assets" %%F in (*) do (
    set "REL=%%F"
    set "REL=!REL:%REPO%\dist\assets\=!"
    set "REL=!REL:\=/!"
    findstr /x /c:"assets/!REL!" "%ENTRIES%" >nul
    if errorlevel 1 (
        set "HINT="
        if not "!REL:/_=!"=="!REL!" set "HINT=   (路径含下划线开头的目录，AGP 会整目录忽略)"
        echo [警告] APK 里缺少资产: assets/!REL!!HINT!
        set /a MISSING+=1
    )
)
if not "!MISSING!"=="0" (
    echo [错误] 有 !MISSING! 个资产没进 APK —— 见上方清单
    goto :fail
)

set "ASSET_COUNT=0"
for /f %%N in ('findstr /b /c:"assets/" "%ENTRIES%" ^| find /c /v ""') do set "ASSET_COUNT=%%N"

:report
echo.
echo ==============================
echo 构建完成！
echo   APK:      %APK%
echo   体积:     %APK_MB% MB
echo   时间:     %APK_TIME%
if not "%ASSET_COUNT%"=="0" echo   资源条目: %ASSET_COUNT% 项
echo ==============================

:: ==========================================
:: 5. 装到设备（可选）
:: ==========================================
if "%DO_INSTALL%"=="0" goto :done

echo.
"%ADB%" get-state 1>nul 2>nul
if errorlevel 1 (
    echo [错误] 没有已连接的设备/模拟器（adb devices 为空）
    goto :fail
)

echo [信息] 安装到设备...
"%ADB%" install -r -t "%APK%"
if errorlevel 1 (
    echo [错误] adb install 失败
    goto :fail
)

"%ADB%" shell am start -n %ACT%
echo.
echo [信息] 已启动。看引擎日志：
echo        "%ADB%" logcat -s GE
echo        "%ADB%" shell run-as %PKG% cat files/GE.log

:done
endlocal
exit /b 0

:: ==========================================
:: 失败退出
:: ==========================================
:fail
echo.
echo [失败] 构建未完成。
endlocal
exit /b 1

:: ==========================================
:: 帮助信息
:: ==========================================
:show_help
echo.
echo 用法: build_android.bat [选项]
echo.
echo 选项:
echo   (无)       - 收资产 -^> 删旧 APK -^> assembleDebug -^> 校验
echo   nopack     - 跳过 gepack（资产没动时用；旧 APK 仍会删，孤儿字节与它无关）
echo   pack       - 强制收资产（默认行为）
echo   install    - 构建完直接装到设备并启动
echo   clean      - 构建前先 gradlew clean（改了构建结构时才需要，很慢）
echo   -h, --help - 显示帮助
echo.
echo 说明:
echo   · 产物: platform\android\app\build\outputs\apk\debug\app-debug.apk
echo   · 需要先跑过一次桌面构建（build.bat）以得到 bin\gepack.exe
echo   · 失败不 pause，直接返回非零退出码，便于终端/CI 调用
echo   · release 变体目前没有配 signingConfig，assembleRelease 出的是 unsigned 包装不上，
echo     要真发布需先在 app\build.gradle 里补 signingConfig
echo.
echo 示例:
echo   build_android.bat                  - 完整构建（改了资产/场景后用这个）
echo   build_android.bat nopack           - 只编 C++ 改动，省掉收资产
echo   build_android.bat nopack install   - 只编 + 装到模拟器
echo   build_android.bat clean            - 改了 Gradle/CMake 结构后
endlocal
exit /b 0
