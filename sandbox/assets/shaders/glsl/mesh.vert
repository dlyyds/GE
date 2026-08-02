#version 450
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

layout (set = 2, binding = 0, std140) uniform ObjectUBO
{
    mat4 model;
    float lodBias;
} object;

layout (location = 0) out vec2 outUV;
layout (location = 1) out float outLodBias;
layout (location = 2) out vec3 outWorldPos;
layout (location = 3) out vec3 outNormal;
layout (location = 4) out vec3 outViewVec;

void main()
{
    outUV = inUV;
    outLodBias = object.lodBias;

    vec4 worldPos = object.model * vec4(inPos, 1.0);
    gl_Position = frame.projection * frame.view * worldPos;

    outWorldPos = worldPos.xyz;
    outNormal = mat3(inverse(transpose(object.model))) * inNormal;
    outViewVec = frame.viewPos.xyz - worldPos.xyz;
}
