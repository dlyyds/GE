#version 460
/* Copyright (c) 2019-2024, Sascha Willems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

layout (set = 1, binding = 0) uniform sampler2D samplerColor;
layout (set = 1, binding = 1) uniform sampler2D samplerNormal;   // 法线贴图（切线空间）
layout (set = 1, binding = 3) uniform sampler2D samplerEmissive; // 自发光贴图

// 每材质 UB（按批次绑定）：存放材质标量参数。
//   params.x = shininess（高光指数，决定高光斑形态/大小）
//   params.y = specularStrength（镜面强度，独立控制高光亮暗）
//   params.z = emissiveStrength（自发光强度，缩放自发光贴图颜色）
//   params.w 预留供后续材质参数扩展。
layout (set = 1, binding = 2, std140) uniform MaterialUBO
{
    vec4 params;
} material;

layout (set = 0, binding = 0, std140) uniform FrameUBO
{
    mat4 projection;
    mat4 view;
    vec4 viewPos;

// 方向光
    vec4 dirLightDirection;
    vec4 dirLightColor;

// 点光源数量（本体在 set 0 binding 1 的 SSBO 中，无上界）
    vec4 lightCount;// x = 点光源数量，yzw 填充对齐

// 环境光
    vec4 ambient;
} frame;

// 点光源 SSBO（set 0, binding 1）：无上界动态数组，解除编译期数量上限。
// 与 C++ 端 Renderer3D::LightGPU 布局一致：vec4 position(xyz=位置, w=半径倒数) + vec4 color(rgb=颜色, a=强度)。
struct PointLight
{
    vec4 position;
    vec4 color;
};
layout (set = 0, binding = 1, std430) readonly buffer LightBuffer
{
    PointLight lights[];
} lightBuffer;

layout (location = 0) in vec2 inUV;
layout (location = 1) in vec3 inWorldPos;
layout (location = 2) in vec3 inNormal;
layout (location = 3) in vec3 inViewVec;
layout (location = 4) in flat vec4 inColor;    // per-instance tint，由顶点着色器传入
layout (location = 5) in vec3 inTangent;       // 世界空间切线
layout (location = 6) in vec3 inBitangent;     // 世界空间副切线

layout (location = 0) out vec4 outFragColor;

/// 计算方向光贡献
/// @param specularStrength 镜面强度系数（独立控制高光亮暗，与 shininess 形态解耦）
vec3 calcDirectionalLight(vec3 N, vec3 V, vec3 albedo, float shininess, float specularStrength)
{
    vec3 L = normalize(-frame.dirLightDirection.xyz);
    vec3 lightColor = frame.dirLightColor.rgb * frame.dirLightColor.w;

    // 漫反射
    float diff = max(dot(N, L), 0.0);
    vec3 diffuse = diff * lightColor;

    // 镜面反射（Blinn-Phong）
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), shininess);
    vec3 specular = spec * lightColor * specularStrength;

    return diffuse * albedo + specular;
}

/// 计算单个点光源贡献（带距离衰减）
/// @param light 点光源（取自 SSBO）
/// @param specularStrength 镜面强度系数（独立控制高光亮暗，与 shininess 形态解耦）
vec3 calcPointLight(PointLight light, vec3 N, vec3 V, vec3 worldPos, vec3 albedo, float shininess, float specularStrength)
{
    vec3 lightPos  = light.position.xyz;
    vec3 lightColor = light.color.rgb * light.color.a;
    float radiusInv = light.position.w;

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
    vec3 specular = spec * lightColor * specularStrength;

    return (diffuse * albedo + specular) * attenuation;
}

void main()
{
    vec4 texColor = texture(samplerColor, inUV, 0.0);
    vec3 albedo = texColor.rgb * inColor.rgb;

    // —— 法线贴图：从切线空间采样并变换到世界空间 ——
    // 采样值 [0,1] 映射到 [-1,1]；用 TBN 矩阵（切线/副切线/法线）变换。
    // 无法线贴图时绑定默认"平坦法线"纹理 (0.5,0.5,1.0)，映射回 (0,0,1)，
    // TBN * (0,0,1) = 几何法线，效果等同未使用法线贴图。
    vec3 tangentNormal = texture(samplerNormal, inUV, 0.0).rgb * 2.0 - 1.0;
    vec3 T = normalize(inTangent);
    vec3 B = normalize(inBitangent);
    vec3 N = normalize(mat3(T, B, normalize(inNormal)) * tangentNormal);

    vec3 V = normalize(inViewVec);
    float shininess = material.params.x;
    float specularStrength = material.params.y;

    // 环境光
    vec3 ambientColor = frame.ambient.rgb * frame.ambient.w;
    vec3 result = ambientColor * albedo;

    // 方向光
    result += calcDirectionalLight(N, V, albedo, shininess, specularStrength);

    // 点光源：循环累加所有点光源的贡献（数量取自 FrameUBO::lightCount）
    for (int i = 0; i < int(frame.lightCount.x); i++) {
        result += calcPointLight(lightBuffer.lights[i], N, V, inWorldPos, albedo, shininess, specularStrength);
    }

    // 自发光：直接加色，不受光照影响。
    // 采样自发光贴图颜色，乘强度参数。无自发光贴图时绑定默认黑色纹理，
    // 采样为 0，不改变结果（物体不发光）。
    vec3 emissive = texture(samplerEmissive, inUV, 0.0).rgb * material.params.z;
    result += emissive;

    outFragColor = vec4(result, 1.0);
}