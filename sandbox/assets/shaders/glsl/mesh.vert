#version 460
/* Copyright (c) 2019-2024, Sascha Willems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// 顶点属性 location 必须与 C++ 端 Vertex 结构体的内存顺序一致：
// Position → Normal → TexCoord
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV;

// 点光源最大数量，必须与 C++ 端 Renderer3D::MAX_POINT_LIGHTS 保持一致
#define MAX_POINT_LIGHTS 8

layout (set = 0, binding = 0, std140) uniform FrameUBO
{
    mat4 projection;
    mat4 view;
    vec4 viewPos;

    // 方向光
    vec4 dirLightDirection;
    vec4 dirLightColor;

    // 点光源数组（每个灯 2 个 vec4：position.w = 半径倒数，color.a = 强度）
    vec4 pointLightPositions[MAX_POINT_LIGHTS];
    vec4 pointLightColors[MAX_POINT_LIGHTS];
    vec4 pointLightCount;   // x = 实际点光源数量，yzw 填充对齐

    // 环境光
    vec4 ambient;
} frame;

// 阶段3（方案B）：per-instance 数据（model + color）存入 SSBO，用 gl_InstanceIndex 索引。
// 采用"块内最后一个 runtime array 成员"写法（而非变量本身是 runtime array），
// 这样是单个 buffer 内的变长数组，不产生 RuntimeDescriptorArray 能力，
// 无需启用 descriptor indexing 设备特性，兼容性更好。
// std430 布局：mat4 = 64B，vec4 = 16B，每实例 80B，与 C++ 端 Renderer3D::InstanceData 一致。
struct InstanceData
{
    mat4 model;
    vec4 color;
};
layout (set = 2, binding = 0, std430) readonly buffer InstanceBuffer
{
    InstanceData instances[];
} instanceBuffer;

layout (location = 0) out vec2 outUV;
layout (location = 1) out vec3 outWorldPos;
layout (location = 2) out vec3 outNormal;
layout (location = 3) out vec3 outViewVec;
layout (location = 4) out flat vec4 outColor;   // per-instance tint，flat 不插值

void main()
{
    outUV = inUV;
    outColor = instanceBuffer.instances[gl_InstanceIndex].color;

    mat4 model = instanceBuffer.instances[gl_InstanceIndex].model;
    vec4 worldPos = model * vec4(inPos, 1.0);
    gl_Position = frame.projection * frame.view * worldPos;

    outWorldPos = worldPos.xyz;
    outNormal = mat3(inverse(transpose(model))) * inNormal;
    outViewVec = frame.viewPos.xyz - worldPos.xyz;
}