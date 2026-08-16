#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
合并金属度 / 粗糙度单通道图为一张 glTF 约定的 metallic-roughness 图。

输出通道约定（glTF / 引擎 PBR 惯例）：
    R = 1（无遮挡贴图时留白，不参与）.  *见下注
    G = 粗糙度 (roughness)
    B = 金属度 (metallic)
    A = 1

用法：
    python merge_metal_rough.py <metalness图> <roughness图> <输出图.png>

示例：
    python tools/merge_metal_rough.py Metal049A_2K-JPG_Metalness.jpg Metal049A_2K-JPG_Roughness.jpg out.png

注：引擎 shader 从 metallic-roughness 图只采样 .g（roughness）与 .b（metallic），
    R 通道留白（255）即可，不影响结果。
"""

import sys
from PIL import Image


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)

    metal_path, rough_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]

    metallic = Image.open(metal_path).convert("L")  # 单通道
    roughness = Image.open(rough_path).convert("L")

    # 尺寸不一致时以较大者为准并放大（一般二者同尺寸，这里仅兜底）
    size = (max(metallic.width, roughness.width),
            max(metallic.height, roughness.height))
    if metallic.size != size:
        metallic = metallic.resize(size)
    if roughness.size != size:
        roughness = roughness.resize(size)

    # 逐像素合并：R=255, G=roughness, B=metallic, A=255
    merged = Image.merge("RGBA", (
        Image.new("L", size, 255),   # R：留白
        roughness,                    # G：粗糙度
        metallic,                     # B：金属度
        Image.new("L", size, 255),    # A：不透明
    ))

    merged.save(out_path)
    print(f"[生成] {out_path} ({size[0]}x{size[1]})")


if __name__ == "__main__":
    main()