#version 460
/* Copyright (c) 2019-2024, Sascha Willems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// 点光源最大数量，必须与 C++ 端 Renderer3D::MAX_POINT_LIGHTS 保持一致
#define MAX_POINT_LIGHTS 8

layout (set = 1, binding = 0) uniform sampler2D samplerColor;

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
    vec4 pointLightCount;// x = 实际点光源数量，yzw 填充对齐

// 环境光
    vec4 ambient;
} frame;

layout (location = 0) in vec2 inUV;
layout (location = 1) in float inLodBias;
layout (location = 2) in vec3 inWorldPos;
layout (location = 3) in vec3 inNormal;
layout (location = 4) in vec3 inViewVec;
layout (location = 5) in flat vec4 inColor;    // per-instance tint，由顶点着色器传入

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

/// 计算单个点光源贡献（带距离衰减）
/// @param index 点光源在数组中的索引
vec3 calcPointLight(int index, vec3 N, vec3 V, vec3 worldPos, vec3 albedo, float shininess)
{
    vec3 lightPos  = frame.pointLightPositions[index].xyz;
    vec3 lightColor = frame.pointLightColors[index].rgb * frame.pointLightColors[index].a;
    float radiusInv = frame.pointLightPositions[index].w;

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
    vec4 texColor = texture(samplerColor, inUV, inLodBias);
    vec3 albedo = texColor.rgb * inColor.rgb;

    vec3 N = normalize(inNormal);
    vec3 V = normalize(inViewVec);
    float shininess = 32.0;

    // 环境光
    vec3 ambientColor = frame.ambient.rgb * frame.ambient.w;
    vec3 result = ambientColor * albedo;

    // 方向光
    result += calcDirectionalLight(N, V, albedo, shininess);

    // 点光源：循环累加所有点光源的贡献
    for (int i = 0; i < int(frame.pointLightCount.x); i++) {
        result += calcPointLight(i, N, V, inWorldPos, albedo, shininess);
    }

    outFragColor = vec4(result, 1.0);
}