#version 450
/**
 * @file sprite.frag
 * @brief 2D 精灵片元着色器（引擎内置版本）
 *
 * 功能：
 *   - 采样 2D 纹理（binding = 1）并与插值后的颜色相乘
 *   - Alpha 测试：低于 alphaThreshold 的像素被 discard
 *   - 预乘 alpha 输出
 */

layout(binding = 0, std140) uniform UniformBlock {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 color;
} u_Block;

layout(binding = 1) uniform sampler2D u_Texture;

layout(location = 0) in vec2 v_uv;
layout(location = 1) out vec4 v_color;

layout(location = 0) out vec4 out_color;

const float alphaThreshold = 0.01;

void main()
{
    vec4 tex_color = texture(u_Texture, v_uv);
    vec4 final_color = tex_color * v_color;

    if (final_color.a < alphaThreshold) {
        discard;
    }

    // 预乘 alpha 输出
    out_color = vec4(final_color.rgb * final_color.a, final_color.a);
}
