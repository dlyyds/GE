#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
环境资产生成脚本。

从源 HDRI 全景图生成一个完整环境所需的 IBL 资产，输出到 assets/environments/<环境名>/：

    skybox.ktx2    天空盒 cubemap（RGBA16F KTX2，cmgen 解 6 面 + ktx 打包）
                   单面尺寸默认 = 源宽/4（原生角度分辨率，见 native_face_size）
    prefilter.ktx  IBL 预滤波镜面图（cmgen --ibl-ld 直接输出的 KTX1，加载已兼容）
    brdf_lut.png   BRDF LUT（共享一份，与环境无关，仅首次生成）
    preview.png    预览缩略图（从源 HDRI 同目录的 .png 复制而来，编辑器下拉框用）

依赖外部工具（可在脚本顶部改路径）：
    cmgen   Filament 离线烘焙工具
    ktx     KTX-Software 统一 CLI

用法：
    python gen_env.py <环境名> <源HDRI路径> [--size 256] [--skybox-size N]

示例：
    python gen_env.py DaySkyHDRI065B assets/environments/DaySkyHDRI065B/source.exr --size 256

注意：已存在的产物会**跳过**，要重新生成需先手动删除对应文件。
"""

import argparse
import pathlib
import shutil
import struct
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


def exr_width(path):
    """只读 OpenEXR 头部取 dataWindow 宽度（不依赖 OpenEXR 模块）。

    EXR 头部是一串 name/type/size/value 属性，直到空 name 结束。dataWindow 的
    值是 4 个 int32（xMin, yMin, xMax, yMax），相减即得分辨率。
    读失败返回 None（此时由调用方退回显式值）。
    """
    try:
        with open(path, "rb") as f:
            if f.read(4) != b"\x76\x2f\x31\x01":  # 0x76 0x2f 0x31 0x01
                return None
            f.read(4)  # version + flags
            while True:
                name = b""
                while True:
                    c = f.read(1)
                    if not c or c == b"\x00":
                        break
                    name += c
                if not name:
                    return None
                while True:
                    c = f.read(1)
                    if not c or c == b"\x00":
                        break
                (ln,) = struct.unpack("<I", f.read(4))
                val = f.read(ln)
                if name == b"dataWindow":
                    x0, _, x1, _ = struct.unpack("<4i", val[:16])
                    return x1 - x0 + 1
    except OSError:
        return None


def native_face_size(source):
    """由源全景图推出天空盒单面的**原生**尺寸 = 源宽 / 4。

    等距柱状全景的横向是 360°，而一个立方体面覆盖 90°，所以单面边长取
    源宽/4 时角度分辨率正好与原图一致。**超过它就是纯插值放大**，文件体积按
    边长平方涨，一个真实像素都不多。

    代价是实打实的：本仓库默认 2048 配 4096×2048 的源，等于 2× 过采样 ——
    单个 skybox.ktx2 因此有 192 MB（占整个资产树 80%），APK 被顶到 245 MB，
    运行期加载还有约 2× 的内存峰值。详见 Android 移植计划书 §10 风险 14。
    """
    w = exr_width(source)
    if not w:
        return None
    n = w // 4
    # 归到 2 的幂并夹在合理区间内（块尺寸/硬件限制都要求 2 的幂）
    p = 1
    while p * 2 <= n:
        p *= 2
    return max(256, min(2048, p))


def run(cmd):
    """执行外部命令，失败即抛错。

    捕获 stdout/stderr 而非直通终端：cmgen 的进度条用 \\r + ANSI 转义刷新，
    在 cmd.exe 等终端会显示成一堆 '?' 刷屏。这里吞掉正常输出，仅在失败时
    回显错误，保证脚本输出干净。
    """
    print("  $ " + " ".join(str(c) for c in cmd))
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise RuntimeError(f"命令失败 (exit {result.returncode}): {cmd[0]}")


def generate_skybox(env_root, source, tmp, skybox_size):
    """cmgen 解 6 面 + ktx 打包成 RGBA16F cubemap，输出 skybox.ktx2。

    skybox_size 默认取源的**原生**分辨率（源宽 / 4，见 native_face_size）——
    再多只是插值放大，体积按平方涨而细节不增。可用 --skybox-size 覆盖。
    """
    out = env_root / "skybox.ktx2"
    if out.exists():
        print(f"  [跳过] 已存在 {out.relative_to(ASSETS_ROOT)}"
              f"（要重新生成请先删掉它）")
        return

    faces_dir = tmp / "faces"
    # cmgen --extract 输出到 <faces_dir>/<源文件名去扩展名>/px.exr ...
    run([CMGEN, "--type=cubemap", "--format=exr", f"--size={skybox_size}",
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


def generate_preview(env_root, source):
    """从源 HDRI 所在目录复制一张预览缩略图到 preview.png。

    源 HDRI（.exr）通常与缩略图 PNG 同在一个文件夹，如
    assets/HDRI/DayEnvironmentHDRI107_4K/ 里既有 *_HDR.exr 也有同名 .png。
    优先取 .png，缺省退回 .jpg（如 *_TONEMAPPED.jpg）。
    """
    out = env_root / "preview.png"
    if out.exists():
        print(f"  [跳过] 已存在 {out.relative_to(ASSETS_ROOT)}")
        return

    src_dir = source.parent
    candidates = sorted(src_dir.glob("*.png")) or sorted(src_dir.glob("*.jpg"))
    if not candidates:
        print(f"  [跳过] 源目录未找到 PNG/JPG 缩略图: {src_dir}")
        return
    # 用 shutil.copy 而非 copyfile：预览图可能很大，走普通复制即可
    shutil.copy(str(candidates[0]), out)
    print(f"  [生成] {out.relative_to(ASSETS_ROOT)} <- {candidates[0].name}")


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
    parser.add_argument("--size", type=int, default=256,
                        help="IBL 预滤波尺寸（默认 256，低频，够用就行）")
    parser.add_argument("--skybox-size", type=int, default=None,
                        help="天空盒 cubemap 单面尺寸（默认按源宽/4 推原生尺寸，"
                             "即角度分辨率与原图一致；超过它只是插值放大）")
    args = parser.parse_args()

    source = pathlib.Path(args.source).resolve()
    if not source.exists():
        sys.exit(f"错误: 源文件不存在: {source}")

    skybox_size = args.skybox_size
    if skybox_size is None:
        skybox_size = native_face_size(source)
        if skybox_size is None:
            print("  [提示] 读不出源的 EXR 分辨率，天空盒退回 --skybox-size 1024")
            skybox_size = 1024
        else:
            print(f"  [推导] 源宽 {exr_width(source)} → 天空盒原生单面 {skybox_size}")

    env_root = ENVIRONMENTS_DIR / args.env_name
    env_root.mkdir(parents=True, exist_ok=True)

    print(f"== 生成环境 {args.env_name} -> {env_root.relative_to(ASSETS_ROOT)} ==")
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="env_gen_"))
    try:
        generate_skybox(env_root, source, tmp, skybox_size)
        generate_prefilter(env_root, source, tmp, args.size)
        generate_preview(env_root, source)
        generate_brdf_lut()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("== 完成 ==")


if __name__ == "__main__":
    main()