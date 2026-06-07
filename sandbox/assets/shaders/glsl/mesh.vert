#version 450
/* Copyright (c) 2019-2024, Sascha Willems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 inNormal;

layout (binding = 0) uniform UBO
{
    mat4 projection;
    mat4 view;
    mat4 model;
    vec4 viewPos;
    float lodBias;
} ubo;

layout (location = 0) out vec2 outUV;
layout (location = 1) out float outLodBias;
layout (location = 2) out vec3 outNormal;
layout (location = 3) out vec3 outViewVec;
layout (location = 4) out vec3 outLightVec;

void main()
{
    outUV = inUV;
    outLodBias = ubo.lodBias;

    vec4 worldPos = ubo.model * vec4(inPos, 1.0);
    gl_Position = ubo.projection * ubo.view * worldPos;

    outNormal = mat3(inverse(transpose(ubo.model))) * inNormal;
    vec3 lightPos = ubo.viewPos.xyz;  // light follows camera
    outLightVec = lightPos - worldPos.xyz;
    outViewVec = ubo.viewPos.xyz - worldPos.xyz;
}
