#version 460
/* Copyright (c) 2019-2024, Sascha Willems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// 调试开关：设 1 时主函数只输出镜面反射（不含漫反射/环境光）。
// 观察：球面出现亮点 → specular 正常；全黑 → specular 异常（排查 BRDF）。
// 排查完记得设回 0。
#define GE_PBR_DEBUG_SPECULAR 1

// —— PBR 片元着色器（Cook-Torrance 金属-粗糙度工作流）——
// 与 mesh.frag 共存：同一套顶点数据 / FrameUBO / 点光源 SSBO / 纹理槽位，
// 只替换光照函数为物理 BRDF。当前为直接光 PBR（方向光 + 点光源），
// 环境光用常量近似；线性空间 / 色调映射（P3）与 IBL（P4）后续阶段接入。

layout (set = 1, binding = 0) uniform sampler2D samplerColor;
layout (set = 1, binding = 1) uniform sampler2D samplerNormal;   // 法线贴图（切线空间）
layout (set = 1, binding = 3) uniform sampler2D samplerEmissive; // 自发光贴图
// 金属-粗糙度贴图（glTF 惯例：B=metallic, G=roughness），PBR 材质使用
layout (set = 1, binding = 4) uniform sampler2D samplerMetallicRoughness;

// 每材质 UB（按批次绑定）：
//   params.x = shininess（Blinn-Phong 高光指数，PBR 下未用）
//   params.y = specularStrength（Blinn-Phong 镜面强度，PBR 下未用）
//   params.z = emissiveStrength（自发光强度，与 Blinn-Phong 一致）
//   params.w 预留。
//   pbr.x = metallic（金属度，0=绝缘体 1=金属）
//   pbr.y = roughness（粗糙度，0=镜面 1=漫）
layout (set = 1, binding = 2, std140) uniform MaterialUBO
{
    vec4 params;
    vec4 pbr;
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

// 点光源 SSBO（set 0, binding 1）：布局与 C++ 端 Renderer3D::LightGPU 一致。
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

const float PI = 3.14159265359;

// 菲涅尔：绝缘体 F0=0.04，金属用 albedo 作为基础反射率
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// GGX 法线分布（Trowbridge-Reitz），roughness 控制高光斑扩散
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Smith 几何遮蔽（直接光用 k=(r+1)²/8），单方向项
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith 几何遮蔽：视线方向 × 光照方向双重遮蔽
float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return geometrySchlickGGX(max(dot(N, V), 0.0), roughness)
         * geometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

// 仅镜面反射（调试用，与 calcDirectLight 的 specular 项一致）
vec3 calcSpecular(vec3 N, vec3 V, vec3 L, vec3 radiance, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    vec3 F0 = vec3(0.04);
    vec3 F  = fresnelSchlick(max(dot(H, V), 0.0), F0);
    float NDF = distributionGGX(N, H, roughness);
    float G   = geometrySmith(N, V, L, roughness);
    return (NDF * G * F) / max(4.0 * NdotV * NdotL, 0.001) * radiance * NdotL;
}

// 单个光源对片元的辐射贡献（Cook-Torrance 反射方程）
/// @param radiance 该光源在片元处的辐射率（含颜色与衰减）
vec3 calcDirectLight(vec3 N, vec3 V, vec3 L, vec3 radiance,
                     vec3 albedo, float metallic, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    // 基础反射率：绝缘体恒 0.04，金属取 albedo
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F  = fresnelSchlick(max(dot(H, V), 0.0), F0);

    float NDF = distributionGGX(N, H, roughness);
    float G   = geometrySmith(N, V, L, roughness);

    // 镜面反射 BRDF（分母保底防除零）
    vec3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, 0.001);

    // 能量守恒：菲涅尔反射掉的能量不能再进入漫反射；金属漫反射为 0
    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    return (kD * albedo / PI + specular) * radiance * NdotL;
}

/// 计算方向光贡献
vec3 calcDirectionalLight(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness)
{
    vec3 L = normalize(-frame.dirLightDirection.xyz);
    vec3 radiance = frame.dirLightColor.rgb * frame.dirLightColor.w;
    return calcDirectLight(N, V, L, radiance, albedo, metallic, roughness);
}

/// 计算单个点光源贡献（带距离衰减）
vec3 calcPointLight(PointLight light, vec3 N, vec3 V, vec3 worldPos, vec3 albedo, float metallic, float roughness)
{
    vec3 lightPos  = light.position.xyz;
    vec3 lightColor = light.color.rgb * light.color.a;
    float radiusInv = light.position.w;

    vec3 L = lightPos - worldPos;
    float dist = length(L);
    L = normalize(L);

    // 距离衰减（平方反比，含半径归一），作为辐射率的一部分
    float attenuation = 1.0 / (1.0 + dist * dist * radiusInv * radiusInv);
    vec3 radiance = lightColor * attenuation;

    return calcDirectLight(N, V, L, radiance, albedo, metallic, roughness);
}

void main()
{
    vec4 texColor = texture(samplerColor, inUV, 0.0);
    vec3 albedo = texColor.rgb * inColor.rgb;

    // —— 法线贴图：从切线空间采样并变换到世界空间 ——
    // 采样值 [0,1] 映射到 [-1,1]；用 TBN 矩阵变换。
    // 无法线贴图时绑定默认"平坦法线"纹理 (0.5,0.5,1.0)，映射回 (0,0,1)，
    // TBN * (0,0,1) = 几何法线，效果等同未使用法线贴图。
    vec3 tangentNormal = texture(samplerNormal, inUV, 0.0).rgb * 2.0 - 1.0;
    vec3 T = normalize(inTangent);
    vec3 B = normalize(inBitangent);
    vec3 N = normalize(mat3(T, B, normalize(inNormal)) * tangentNormal);

    vec3 V = normalize(inViewVec);

    // 金属-粗糙度：贴图 B/G 通道 × 标量系数。无 MR 贴图时绑定默认 (G=1,B=1)
    // 纹理，两者乘以 1 即等于标量 pbr 系数原值（标量 fallback）。
    vec4 mr = texture(samplerMetallicRoughness, inUV, 0.0);
    float metallic  = mr.b * material.pbr.x;
    float roughness = mr.g * material.pbr.y;

#if GE_PBR_DEBUG_SPECULAR
    // ── 调试：输出点光源数量 lightCount ──
    //   灰≈0.8(33盏) → 灯已传入；纯黑 → lightCount=0（灯没进 SSBO/UBO）。
    outFragColor = vec4(vec3(frame.lightCount.x / 40.0), 1.0);
    return;
#endif

    // 环境光：P0 用常量近似（后续 P4 替换为 IBL）
    vec3 ambientColor = frame.ambient.rgb * frame.ambient.w;
    vec3 result = ambientColor * albedo;

    // 方向光
    result += calcDirectionalLight(N, V, albedo, metallic, roughness);

    // 点光源：循环累加所有点光源的贡献（数量取自 FrameUBO::lightCount）
    for (int i = 0; i < int(frame.lightCount.x); i++) {
        result += calcPointLight(lightBuffer.lights[i], N, V, inWorldPos, albedo, metallic, roughness);
    }

    // 自发光：直接加色，不受光照影响。
    vec3 emissive = texture(samplerEmissive, inUV, 0.0).rgb * material.params.z;
    result += emissive;

    outFragColor = vec4(result, 1.0);
}