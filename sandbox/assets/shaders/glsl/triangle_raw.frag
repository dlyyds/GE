#version 450

// 最简单的不带任何 UBO/纹理的片元着色器
// 直接输出插值后的顶点颜色

layout(location = 0) in vec3 v_color;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(v_color, 1.0);
}
