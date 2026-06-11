#version 450
/* Copyright (c) 2019-2024, Sascha Willems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 inNormal;

layout (set = 0, binding = 0, std140) uniform FrameUBO
{
    mat4 projection;
    mat4 view;
    vec4 viewPos;
} frame;

layout (set = 2, binding = 0, std140) uniform ObjectUBO
{
    mat4 model;
    float lodBias;
} object;

layout (location = 0) out vec2 outUV;
layout (location = 1) out float outLodBias;
layout (location = 2) out vec3 outNormal;
layout (location = 3) out vec3 outViewVec;
layout (location = 4) out vec3 outLightVec;

void main()
{
    outUV = inUV;
    outLodBias = object.lodBias;

    vec4 worldPos = object.model * vec4(inPos, 1.0);
    gl_Position = frame.projection * frame.view * worldPos;

    outNormal = mat3(inverse(transpose(object.model))) * inNormal;
    vec3 lightPos = frame.viewPos.xyz;  // light follows camera
    outLightVec = lightPos - worldPos.xyz;
    outViewVec = frame.viewPos.xyz - worldPos.xyz;
}
