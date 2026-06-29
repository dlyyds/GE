#version 450

// 最简单的不带任何 UBO/纹理的顶点着色器
// 直接接收顶点位置和颜色，输出到片元着色器

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec3 in_color;

layout(location = 0) out vec3 v_color;

void main() {
    gl_Position = vec4(in_position, 0.0, 1.0);
    v_color = in_color;
}
