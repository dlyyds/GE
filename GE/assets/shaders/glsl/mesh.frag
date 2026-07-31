#version 450
/* Copyright (c) 2019-2024, Sascha Willems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout (set = 1, binding = 0) uniform sampler2D samplerColor;

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

layout (location = 0) in vec2 inUV;
layout (location = 1) in float inLodBias;
layout (location = 2) in vec3 inWorldPos;
layout (location = 3) in vec3 inNormal;
layout (location = 4) in vec3 inViewVec;

layout (location = 0) out vec4 outFragColor;

/// 计算方向光贡献
vec3 calcDirectionalLight(vec3 N, vec3 V, vec3 albedo, float shininess)
{
    vec3 L = normalize(-frame.dirLightDirection.xyz);
    vec3 lightColor = frame.dirLightColor.rgb * frame.dirLightColor.w;

    // 漫反射
    float diff = max(dot(N, L), 0.0);
    vec3 diffuse = diff * lightColor;

    // 镜面反射（Blinn-Phong）
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), shininess);
    vec3 specular = spec * lightColor * 0.5;

    return diffuse * albedo + specular;
}

/// 计算点光源贡献（带距离衰减）
vec3 calcPointLight(vec3 N, vec3 V, vec3 worldPos, vec3 albedo, float shininess)
{
    vec3 lightPos = frame.pointLightPosition.xyz;
    vec3 lightColor = frame.pointLightColor.rgb * frame.pointLightColor.w;
    float radiusInv = frame.pointLightPosition.w;

    vec3 L = lightPos - worldPos;
    float dist = length(L);
    L = normalize(L);

    // 距离衰减：1 / (1 + d^2 * radiusInv^2)
    float attenuation = 1.0 / (1.0 + dist * dist * radiusInv * radiusInv);

    // 漫反射
    float diff = max(dot(N, L), 0.0);
    vec3 diffuse = diff * lightColor;

    // 镜面反射
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), shininess);
    vec3 specular = spec * lightColor * 0.5;

    return (diffuse * albedo + specular) * attenuation;
}

void main()
{
    vec4 color = texture(samplerColor, inUV, inLodBias);
    vec3 albedo = color.rgb;

    vec3 N = normalize(inNormal);
    vec3 V = normalize(inViewVec);
    float shininess = 32.0;

    // 环境光
    vec3 ambientColor = frame.ambient.rgb * frame.ambient.w;
    vec3 result = ambientColor * albedo;

    // 方向光
    result += calcDirectionalLight(N, V, albedo, shininess);

    // 点光源
    result += calcPointLight(N, V, inWorldPos, albedo, shininess);

    outFragColor = vec4(result, 1.0);
}
