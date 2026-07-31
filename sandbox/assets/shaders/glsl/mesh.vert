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

layout (set = 0, binding = 0, std140) uniform FrameUBO
{
    mat4 projection;
    mat4 view;
    vec4 viewPos;

    // 方向光
    vec4 dirLightDirection;
    vec4 dirLightColor;
    // 点光源
    vec4 pointLightPosition;
    vec4 pointLightColor;
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
