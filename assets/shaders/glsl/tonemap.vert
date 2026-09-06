#version 460

// Tonemap 顶点着色器：与延迟 Lighting / 天空盒一样绘制全屏三角形，无需顶点缓冲。
layout (location = 0) out vec2 outUV;

void main()
{
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0, 1.0);
}
