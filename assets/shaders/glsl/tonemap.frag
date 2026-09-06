#version 460

// HDR 中间缓冲 Tonemap 片元着色器。
// 采样 Lighting（阶段 B 后还包括 Transparent）写入的 RGBA16F HDR 缓冲。
// HDR.a 用作天空/几何元数据：
//   - < 0.5：天空像素（或未合成背景），直接输出 RGB，不做曝光/tonemap；
//   - >= 0.5：场景像素，应用曝光 + ACES 后输出。
// 输出总是 alpha = 1，保持现有 ViewportColor 语义（ImGui / Scene2D 使用 RGB）。

layout (set = 0, binding = 0, std140) uniform TonemapUBO
{
    vec4 exposure; // x = 曝光系数（默认 1.0），yzw 预留
    vec4 flags;    // x = tonemap 开关，y = 启用天空 alpha 旗标，zw 预留
} tonemap;

layout (set = 0, binding = 1) uniform sampler2D samplerHDR;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

vec3 acesToneMap(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) /
                 (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    vec4 hdr = texture(samplerHDR, inUV);
    vec3 color = hdr.rgb;

    if (tonemap.flags.y > 0.5 && hdr.a < 0.5)
    {
        // 天空像素：直接输出，不曝光不 tonemap（维持现状语义）
        outColor = vec4(color, 1.0);
        return;
    }

    color *= tonemap.exposure.x;
    if (tonemap.flags.x > 0.5)
    {
        color = acesToneMap(color);
    }

    outColor = vec4(color, 1.0);
}
