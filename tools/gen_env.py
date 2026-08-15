#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
环境资产生成脚本。

从源 HDRI 全景图生成一个完整环境所需的 IBL 资产，输出到 assets/environments/<环境名>/：

    skybox.ktx2    天空盒 cubemap（RGBA16F KTX2，cmgen 解 6 面 + ktx 打包）
    prefilter.ktx  IBL 预滤波镜面图（cmgen --ibl-ld 直接输出的 KTX1，加载已兼容）
    brdf_lut.png   BRDF LUT（共享一份，与环境无关，仅首次生成）

依赖外部工具（可在脚本顶部改路径）：
    cmgen   Filament 离线烘焙工具
    ktx     KTX-Software 统一 CLI

用法：
    python gen_env.py <环境名> <源HDRI路径> [--size 256]

示例：
    python gen_env.py DaySkyHDRI065B assets/environments/DaySkyHDRI065B/source.exr --size 256
"""

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------
# 工具路径（按本机实际安装位置修改）
# ---------------------------------------------------------------------------
CMGEN = r"E:/software/filament/bin/cmgen.exe"
KTX = r"E:/software/KTX-Software/bin/ktx.exe"

# 资产根目录 = 本脚本所在目录的上一级的 assets/
ASSETS_ROOT = pathlib.Path(__file__).resolve().parent.parent / "assets"
ENVIRONMENTS_DIR = ASSETS_ROOT / "environments"

# ktx create 的 cubemap 面顺序 = Vulkan (+X, -X, +Y, -Y, +Z, -Z)
FACE_ORDER = ["px", "nx", "py", "ny", "pz", "nz"]


def run(cmd):
    """执行外部命令，失败即抛错。"""
    print("  $ " + " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True)


def generate_skybox(env_root, source, tmp, size):
    """cmgen 解 6 面 + ktx 打包成 RGBA16F cubemap，输出 skybox.ktx2。"""
    out = env_root / "skybox.ktx2"
    if out.exists():
        print(f"  [跳过] 已存在 {out.relative_to(ASSETS_ROOT)}")
        return

    faces_dir = tmp / "faces"
    # cmgen --extract 输出到 <faces_dir>/<源文件名去扩展名>/px.exr ...
    run([CMGEN, "--type=cubemap", "--format=exr", f"--size={size}",
         f"--extract={faces_dir}", str(source)])

    # 定位解出的 6 面（cmgen 放在以源文件名命名的子目录里）
    stem = source.stem
    face_paths = [faces_dir / stem / f"{f}.exr" for f in FACE_ORDER]
    missing = [p for p in face_paths if not p.exists()]
    if missing:
        raise FileNotFoundError(f"cmgen 未解出完整 6 面，缺失: {missing[0]}")

    run([KTX, "create", "--cubemap", "--format", "R16G16B16A16_SFLOAT",
         *map(str, face_paths), str(out)])
    print(f"  [生成] {out.relative_to(ASSETS_ROOT)}")


def generate_prefilter(env_root, source, tmp, size):
    """cmgen --ibl-ld 生成预滤波镜面图 KTX1，输出 prefilter.ktx。"""
    out = env_root / "prefilter.ktx"
    if out.exists():
        print(f"  [跳过] 已存在 {out.relative_to(ASSETS_ROOT)}")
        return

    ld_dir = tmp / "ld"
    run([CMGEN, f"--ibl-ld={ld_dir}", f"--size={size}", "--format=ktx", str(source)])

    # cmgen 在 <ld_dir> 下输出 <ld_dir名>_ibl.ktx
    ibl = ld_dir / f"{ld_dir.name}_ibl.ktx"
    if not ibl.exists():
        raise FileNotFoundError(f"cmgen 未生成预滤波图: {ibl}")
    # 用 shutil.move 而非 rename：临时目录在系统盘、目标在资源盘时
    # os.rename 跨磁盘会报 OSError(18)，shutil.move 会自动复制+删除。
    shutil.move(str(ibl), out)
    print(f"  [生成] {out.relative_to(ASSETS_ROOT)}")


def generate_brdf_lut():
    """BRDF LUT 与环境无关，共享一份在 environments/ 根，仅首次生成。"""
    out = ENVIRONMENTS_DIR / "brdf_lut.png"
    if out.exists():
        print(f"  [跳过] 共享 BRDF LUT 已存在 {out.relative_to(ASSETS_ROOT)}")
        return
    run([CMGEN, f"--ibl-dfg={out}", "--size=256"])
    print(f"  [生成] {out.relative_to(ASSETS_ROOT)}")


def main():
    parser = argparse.ArgumentParser(description="生成环境资产（天空盒 + 预滤波 + BRDF LUT）")
    parser.add_argument("env_name", help="环境名，作为 environments/ 下的子文件夹名")
    parser.add_argument("source", help="源 HDRI 全景图路径（.exr）")
    parser.add_argument("--size", type=int, default=256, help="cubemap 单面尺寸（默认 256）")
    args = parser.parse_args()

    source = pathlib.Path(args.source).resolve()
    if not source.exists():
        sys.exit(f"错误: 源文件不存在: {source}")

    env_root = ENVIRONMENTS_DIR / args.env_name
    env_root.mkdir(parents=True, exist_ok=True)

    print(f"== 生成环境 {args.env_name} -> {env_root.relative_to(ASSETS_ROOT)} ==")
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="env_gen_"))
    try:
        generate_skybox(env_root, source, tmp, args.size)
        generate_prefilter(env_root, source, tmp, args.size)
        generate_brdf_lut()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("== 完成 ==")


if __name__ == "__main__":
    main()