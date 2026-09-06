#version 460
#define HAS_IBL 1
/* Copyright (c) 2019-2024, Sascha Willems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// —— PBR IBL HDR 透明合成片元着色器（Cook-Torrance + split-sum IBL）——
// 与 mesh.frag 共存：同一套顶点数据 / FrameUBO / 点光源 SSBO / 纹理槽位，
// 只替换光照函数为物理 BRDF。当前为直接光 PBR（方向光 + 点光源），
// 与 mesh_pbr_ibl 相同，但输出线性 HDR（不做 ACES），供 Transparent(HDR) 在 Tonemap 前合成。

layout (set = 1, binding = 0) uniform sampler2D samplerColor;
layout (set = 1, binding = 1) uniform sampler2D samplerNormal;// 法线贴图（切线空间）
layout (set = 1, binding = 3) uniform sampler2D samplerEmissive;// 自发光贴图
// 金属-粗糙度贴图（glTF 惯例：B=metallic, G=roughness），PBR 材质使用
layout (set = 1, binding = 4) uniform sampler2D samplerMetallicRoughness;

// —— IBL 环境光三件套（仅 HAS_IBL 变体声明并采样）——
// 辐照度采样器复用预滤波 cubemap：漫反射取最高 mip（近似余弦卷积），
// 镜面按粗糙度取 mip。BRDF LUT 是 2D split-sum 表，轴 (NoV, roughness)。
#ifdef HAS_IBL
layout (set = 1, binding = 5) uniform samplerCube samplerIrradiance;// 漫反射辐照度（= 预滤波图最高 mip）
layout (set = 1, binding = 6) uniform samplerCube samplerPrefilter; // 镜面预滤波 mip 链 cubemap
layout (set = 1, binding = 7) uniform sampler2D  samplerBrdfDFG;    // BRDF LUT（2D，(NoV, roughness)）
#endif

// 每材质 UB（按批次绑定）：
//   params.x = shininess（Blinn-Phong 高光指数，PBR 下未用）
//   params.y = specularStrength（Blinn-Phong 镜面强度，PBR 下未用）
//   params.z = alphaCutoff（MASK 裁剪阈值；Opaque/Blend 传 -1 关闭 discard）
//   params.w = uvTiling（纹理平铺 / UV 缩放密度，采样前乘 inUV）
//   pbr.x = metallic（金属度，0=绝缘体 1=金属）
//   pbr.y = roughness（粗糙度，0=镜面 1=漫）
//   emissiveFactor.rgb = 自发光颜色因子（乘自发光贴图颜色）
//   emissiveFactor.w = 材质基础 alpha（baseAlpha，glTF baseColorFactor[3] / OBJ dissolve）
layout (set = 1, binding = 2, std140) uniform MaterialUBO
{
    vec4 params;
    vec4 pbr;
    vec4 emissiveFactor;
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

// IBL 参数（仅 PBR-IBL 变体 HAS_IBL 使用）：x = 预滤波最大 mip 数（MAX_REFLECTION_LOD），
    // y = IBL 环境光强度（整体缩放 diffuse + specular 贡献）
    vec4 iblParams;
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
layout (location = 4) in flat vec4 inColor;// per-instance tint，由顶点着色器传入
layout (location = 5) in vec3 inTangent;// 世界空间切线
layout (location = 6) in vec3 inBitangent;// 世界空间副切线

layout (location = 0) out vec4 outFragColor;

const float PI = 3.14159265359;

// ACES（Academy Color Encoding System）近似色调映射：
// 将线性 HDR 结果压入 [0,1]，高光柔化、暗部保留层次，避免纯白过曝。
vec3 acesToneMap(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) /
                 (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

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
    // Albedo 贴图以 sRGB 格式加载，硬件采样时自动解码到线性空间，PBR 光照数学才成立。
    // 法线 / metallic / roughness 是数据而非颜色，仍用 Unorm 不做 gamma 解码。
    // 保留完整 texColor（含 a），供收尾算最终 alpha（纹理 alpha × tint alpha）。
    vec4 texColor = texture(samplerColor, inUV * material.params.w, 0.0);
    vec3 albedo = texColor.rgb * inColor.rgb;

    // —— 法线贴图：从切线空间采样并变换到世界空间 ——
    // 采样值 [0,1] 映射到 [-1,1]；用 TBN 矩阵变换。
    // 无法线贴图时绑定默认"平坦法线"纹理 (0.5,0.5,1.0)，映射回 (0,0,1)，
    // TBN * (0,0,1) = 几何法线，效果等同未使用法线贴图。
    vec3 tangentNormal = texture(samplerNormal, inUV * material.params.w, 0.0).rgb * 2.0 - 1.0;
    vec3 T = normalize(inTangent);
    vec3 B = normalize(inBitangent);
    vec3 N = normalize(mat3(T, B, normalize(inNormal)) * tangentNormal);

    vec3 V = normalize(inViewVec);

    // 金属-粗糙度：贴图 B/G 通道 × 标量系数。无 MR 贴图时绑定默认 (G=1,B=1)
    // 纹理，两者乘以 1 即等于标量 pbr 系数原值（标量 fallback）。
    vec4 mr = texture(samplerMetallicRoughness, inUV * material.params.w, 0.0);
    float metallic  = mr.b * material.pbr.x;
    float roughness = mr.g * material.pbr.y;

    // 环境光：P0 用常量近似；P4（HAS_IBL）用 split-sum IBL 替换。
#ifdef HAS_IBL
    // —— 环境光：split-sum IBL（Filament 式三图成套采样）——
    // F0 提到 main：绝缘体恒 0.04，金属取 albedo（直接光与 IBL 共用）
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NoV = max(dot(N, V), 0.0);

    // 粗糙度感知菲涅尔（IBL 用）：粗糙表面 F0 随粗糙度向 1 靠拢，避免过暗
    vec3 F = F0 + (max(vec3(1.0 - roughness), F0) - F0)
            * pow(clamp(1.0 - NoV, 0.0, 1.0), 5.0);
    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    // 漫反射：辐照度（预滤波图最高 mip，近似余弦卷积）按法线采样
    vec3 irradiance = textureLod(samplerIrradiance, N, frame.iblParams.x).rgb;
    vec3 diffuse = irradiance * albedo * kD;

    // 镜面：反射方向查预滤波 mip，粗糙度选层级
    vec3 R = reflect(-V, N);
    vec3 prefiltered = textureLod(samplerPrefilter, R,
                                  roughness * frame.iblParams.x).rgb;
    // BRDF LUT：.r = F0 系数（乘 F），.g = 菲涅尔尾项（直接加）
    vec2 brdf = texture(samplerBrdfDFG, vec2(NoV, roughness)).rg;
    vec3 specular = prefiltered * (F * brdf.r + brdf.g);

    // iblParams.y = IBL 环境光强度（整体缩放 diffuse + specular 贡献）
    vec3 result = (diffuse + specular) * frame.iblParams.y;
#else
    vec3 ambientColor = frame.ambient.rgb * frame.ambient.w;
    vec3 result = ambientColor * albedo;
#endif

    // 方向光
    result += calcDirectionalLight(N, V, albedo, metallic, roughness);

    // 点光源：循环累加所有点光源的贡献（数量取自 FrameUBO::lightCount）
    for (int i = 0; i < int(frame.lightCount.x); i++) {
        result += calcPointLight(lightBuffer.lights[i], N, V, inWorldPos, albedo, metallic, roughness);
    }

    // 自发光：直接加色，不受光照影响。
    // 最终自发光颜色 = 贴图采样颜色 × emissiveFactor（glTF 惯例）。
    vec3 emissive = texture(samplerEmissive, inUV * material.params.w, 0.0).rgb * material.emissiveFactor.rgb;
    result += emissive;

    // 输出前：ACES 色调映射（线性 HDR → [0,1]）后直接输出线性值，由 sRGB
    // swapchain 硬件编码回显示空间；若在此再手动 gamma 编码会与硬件叠加成
    // 双重编码，画面偏亮发白。
    // HDR 透明路径：保留线性 HDR，ACES 由最后的 Tonemap pass 统一执行。

    // —— 透明度（alphaMode 语义，值经 Renderer3D 填于 material.params.z / .w）——
    // 最终 alpha = 纹理 alpha × 材质基础 alpha（emissiveFactor.w，baseAlpha）× 实例 tint alpha
    // （inColor.a，DrawMesh 的 color.a）。MASK 裁剪：params.z >= 0 表示启用裁剪
    // （Opaque/Blend 传 -1），低于阈值 discard。混合在线性空间进行（ACES 只作用于
    // RGB，alpha 不经过 tone map），结果输出后由 sRGB swapchain 硬件编码。
    // 是否混合由管线混合状态决定，此处仅恒输出真 alpha。
    float alpha = texColor.a * material.emissiveFactor.w * inColor.a;
    if (material.params.z >= 0.0 && alpha < material.params.z) discard;
    outFragColor = vec4(result, alpha);
}