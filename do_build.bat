@echo off
call "F:\c++budiltool\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
set PATH=F:\c++budiltool\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
ninja -C F:\yxy\project\GameEngine\bin-int-msvc -j4 > F:\yxy\project\GameEngine\build_output.txt 2>&1
echo DONE
