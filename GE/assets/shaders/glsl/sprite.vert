#version 450
/**
 * @file sprite.vert
 * @brief 2D 精灵顶点着色器（引擎内置版本）
 *
 * 顶点输入：
 *   location 0 : vec3 位置（xyz，世界坐标）
 *   location 1 : vec2 纹理坐标（uv）
 *   location 2 : vec4 顶点颜色（rgba）
 *
 * Uniform 块（binding = 0，std140）：
 *   view       - 视图矩阵（2D 模式下为单位矩阵）
 *   projection - 投影矩阵
 *   color      - 整体叠加颜色（tint）
 *
 * 输出到片元着色器：
 *   v_uv    - 插值后的纹理坐标
 *   v_color - 插值后的顶点颜色
 *
 * 说明：模型变换已在 CPU 端烘焙到顶点位置中，
 *       着色器只负责 view × projection 变换。
 */

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(binding = 0, std140) uniform UniformBlock {
    mat4 view;
    mat4 projection;
    vec4 color;
} u_Block;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main()
{
    gl_Position = u_Block.projection * u_Block.view * vec4(in_position, 1.0);
    v_uv = in_uv;
    v_color = in_color * u_Block.color;
}
