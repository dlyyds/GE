#version 460

// 天空盒顶点着色器：全屏三角形（无顶点缓冲）。
// 由 gl_VertexIndex 生成 3 个顶点，覆盖整个视口：
//   v0: (0,0) → NDC(-1,-1)
//   v1: (2,0) → NDC( 1,-1)
//   v2: (0,2) → NDC(-1, 1)
// 三角形在屏幕外延伸，光栅化时覆盖全部像素，无需顶点缓冲与索引缓冲。

layout (location = 0) out vec2 outUV;

void main()
{
    // 位运算技巧生成三角形三个顶点的 UV：(0,0)/(2,0)/(0,2)
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0, 1.0);
}