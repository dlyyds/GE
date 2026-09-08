#version 460
/* —— 水面片元着色器（HDR 透明变体，不做 ACES）——
 * 供延迟 HDR 链的透明段使用：写 Scene_HDR，交给最后 Tonemap 统一色调映射。
 * 内容与 water.frag 完全相同，仅去掉 acesToneMap。
 */

layout(set = 0, binding = 0, std140) uniform FrameUBO
{
    mat4 projection;
    mat4 view;
    vec4 viewPos;
    vec4 dirLightDirection;
    vec4 dirLightColor;
    vec4 lightCount;
    vec4 ambient;
    vec4 iblParams;
} frame;

struct PointLight
{
    vec4 position;
    vec4 color;
};
layout(set = 0, binding = 1, std430) readonly buffer LightBuffer
{
    PointLight lights[];
} lightBuffer;

layout(set = 0, binding = 2, std140) uniform WaterUBO
{
    mat4 model;
    vec4 timeParams;
    vec4 deepColor;
    vec4 shallowColor;
    vec4 sizeParams;
    vec4 foamParams;
    vec4 waves[4];
    vec4 waveSpeeds[4];
    vec4 colorParams; // x=色彩平铺, y=色彩强度
} water;

layout(set = 1, binding = 0) uniform sampler2D samplerNormal;
layout(set = 1, binding = 1) uniform samplerCube samplerPrefilter;
layout(set = 1, binding = 2) uniform sampler2D samplerColor; // 色彩/固有色贴图（可选）

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec3 inWorldPos;
layout(location = 2) in vec3 inNormal;

layout(location = 0) out vec4 outFragColor;

const float PI = 3.14159265359;

// 反射增益系数：整体放大 IBL 反射的最终强度（调试/美术需要时调大，1.0 = 原样）。
// 影响反射清晰观感的还有：视角掠射角（Fresnel）、Roughness（越低反射越锐）、
// 环境组件 IBL 强度。反射总亮度 = 预滤波 × IBL强度 × 反射强度 × 本系数。
const float kReflectionGain = 1.6;

float distributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdH = max(dot(N, H), 0.0);
    float d = NdH * NdH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float geometrySchlickGGX(float NdV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdV / (NdV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    return geometrySchlickGGX(max(dot(N, V), 0.0), roughness)
         * geometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main()
{
    vec3 geoN = normalize(inNormal);
    vec3 detail = texture(samplerNormal, inUV * water.sizeParams.z).rgb * 2.0 - 1.0;
    vec3 N = normalize(geoN + vec3(detail.x, 0.0, detail.y) * water.timeParams.y);

    vec3 V = normalize(frame.viewPos.xyz - inWorldPos);
    float NoV = clamp(dot(N, V), 0.0, 1.0);

    const float F0 = 0.02;
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0);

    vec3 reflCol = vec3(0.0);
    if (frame.iblParams.x > 0.0) {
        vec3 R = reflect(-V, N);
        float roughLod = water.timeParams.z * frame.iblParams.x;
        reflCol = textureLod(samplerPrefilter, R, roughLod).rgb
                * frame.iblParams.y * water.sizeParams.w * kReflectionGain;
    }

    // 底色：深水色与色彩贴图按强度 mix（未贴图时强度 0 → 纯深水色，行为不变）。
    vec3 mapColor = texture(samplerColor, inUV * water.colorParams.x).rgb;
    vec3 waterColor = mix(water.deepColor.rgb, mapColor, water.colorParams.y);

    vec3 L = normalize(-frame.dirLightDirection.xyz);
    float NdL = max(dot(N, L), 0.0);
    vec3 H = normalize(L + V);
    float D = distributionGGX(N, H, water.timeParams.z);
    float G = geometrySmith(N, V, L, water.timeParams.z);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), vec3(F0));
    vec3 specular = (D * G * F) / max(4.0 * NoV * NdL, 0.001);

    vec3 result = waterColor
                  * (frame.ambient.rgb * frame.ambient.w
                     + frame.dirLightColor.rgb * frame.dirLightColor.w * NdL)
                  + frame.dirLightColor.rgb * frame.dirLightColor.w * specular;

    for (int i = 0; i < int(frame.lightCount.x); ++i) {
        vec3 lp = lightBuffer.lights[i].position.xyz;
        vec3 lc = lightBuffer.lights[i].color.rgb * lightBuffer.lights[i].color.a;
        float radiusInv = lightBuffer.lights[i].position.w;
        vec3 toL = lp - inWorldPos;
        float dist = length(toL);
        vec3 lDir = toL / max(dist, 0.0001);
        float atten = 1.0 / (1.0 + dist * dist * radiusInv * radiusInv);
        float ldL = max(dot(N, lDir), 0.0);
        vec3 pH = normalize(lDir + V);
        float pD = distributionGGX(N, pH, water.timeParams.z);
        float pG = geometrySmith(N, V, lDir, water.timeParams.z);
        vec3 pF = fresnelSchlick(max(dot(pH, V), 0.0), vec3(F0));
        vec3 pSpec = (pD * pG * pF) / max(4.0 * NoV * ldL, 0.001);
        result += (waterColor * ldL + pSpec) * lc * atten;
    }

    result += reflCol * fresnel;

    // HDR 透明：保留线性 HDR 输出，ACES 由最后 Tonemap pass 统一执行。
    // 透明度与 water.frag 一致：Fresnel 提供视角轮廓 + 不透明度做主控。
    // 管线以 SRC_ALPHA / ONE_MINUS_SRC_ALPHA 混合；alpha 通道同时承担
    // Scene_HDR 的「天空/几何元数据」：dstAlpha=eOneMinusSrcAlpha 使其自适应，
    // 底下是不透明几何 → 收敛到 1（Tonemap 当几何、正常曝光），底下是天空 →
    // 保留片元 alpha，透到只剩天空时被 Tonemap 当天空直出（语义一致）。
    float fresnelProfile = clamp(fresnel + 0.1, 0.0, 1.0);
    float alpha = clamp(water.deepColor.a * (0.25 + 0.75 * fresnelProfile), 0.0, 1.0);
    outFragColor = vec4(result, alpha);
}
