#version 450
/**
 * @file sprite.frag
 * @brief 2D 精灵片元着色器
 *
 * 功能：
 *   - 采样 2D 纹理（binding = 1）并与插值后的颜色相乘
 *   - 支持 Alpha 测试：低于 alphaThreshold 的像素被 discard（做硬边透明）
 *   - 预乘 alpha 输出，配合预乘 alpha 混合模式获得正确的透明叠加效果
 *
 * 如果未绑定纹理或纹理为空，可作为纯色矩形使用（纹理采样结果为 vec4(1.0)）。
 */

layout(binding = 0, std140) uniform UniformBlock {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 color;
} u_Block;

layout(binding = 1) uniform sampler2D u_Texture;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

/// Alpha 测试阈值（0 = 关闭；> 0 时低于阈值的像素被丢弃）
const float alphaThreshold = 0.01;

void main()
{
    vec4 tex_color = texture(u_Texture, v_uv);
    vec4 final_color = tex_color * v_color;

    // Alpha 测试：剔除接近完全透明的像素（避免写入深度 / 颜色缓冲）
    if (final_color.a < alphaThreshold) {
        discard;
    }

    // 预乘 alpha 输出 —— 配合 SrcAlpha, OneMinusSrcAlpha 混合时直接用即可；
    // 若使用预乘 alpha 混合模式（src = VK_BLEND_FACTOR_ONE, dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA），
    // 效果更正确（边缘无黑边）。
    out_color = vec4(final_color.rgb * final_color.a, final_color.a);
}
