#version 460
/* —— 水面片元着色器（前向，带 ACES 色调映射）——
 * Fresnel 反射 + IBL 预滤波 + 方向光/点光 GGX 高光 + 法线贴图扰动。
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
} water;

layout(set = 1, binding = 0) uniform sampler2D samplerNormal;
layout(set = 1, binding = 1) uniform samplerCube samplerPrefilter;

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec3 inWorldPos;
layout(location = 2) in vec3 inNormal;

layout(location = 0) out vec4 outFragColor;

const float PI = 3.14159265359;

vec3 acesToneMap(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) /
                 (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

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
    // —— 法线：波顶点法线 + 法线贴图细节（扰动 XZ 平面，保持 +Y 上）——
    vec3 geoN = normalize(inNormal);
    vec3 detail = texture(samplerNormal, inUV * water.sizeParams.z).rgb * 2.0 - 1.0;
    vec3 N = normalize(geoN + vec3(detail.x, 0.0, detail.y) * water.timeParams.y);

    vec3 V = normalize(frame.viewPos.xyz - inWorldPos);
    float NoV = clamp(dot(N, V), 0.0, 1.0);

    // —— Fresnel（绝缘体 F0≈0.02）——
    const float F0 = 0.02;
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0);

    // —— 反射：IBL 预滤波（frame.iblParams.x > 0 表示环境图可用）——
    vec3 reflCol = vec3(0.0);
    if (frame.iblParams.x > 0.0) {
        vec3 R = reflect(-V, N);
        float roughLod = water.timeParams.z * frame.iblParams.x;
        reflCol = textureLod(samplerPrefilter, R, roughLod).rgb
                * frame.iblParams.y * water.sizeParams.w;
    }

    // 阶段 1：无场景深度，水色先取深水色；阶段 2 替换为折射/深度吸收。
    vec3 waterColor = water.deepColor.rgb;

    // —— 方向光 ——
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

    // —— 点光源（复用 LightBuffer）——
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

    // 反射按 Fresnel 混入
    result += reflCol * fresnel;

    // 水面透明：按 Fresnel 决定 alpha，垂直看较透、掠射角较实。
    // water.deepColor.a 是整体不透明度（编辑器「不透明度」），与之相乘后再钳制。
    float alpha = clamp((fresnel + 0.1) * water.deepColor.a, 0.0, 1.0);

    // 前向路径：ACES 色调映射后再输出（由 sRGB swapchain 硬件编码）
    outFragColor = vec4(acesToneMap(result), alpha);
}