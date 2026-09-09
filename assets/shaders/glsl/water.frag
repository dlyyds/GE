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

    // —— 相机在水面以下（片元为背面）时的双面处理 ——
    // 水格几何法线恒朝 +Y：若背面片元仍按朝上法线计算，NoV 会被钳到 0 →
    // Fresnel 恒 1 → alpha 饱和成实墙（从水下往上看水面不透明）。这里把着色
    // 法线翻转朝向相机，让两侧的 Fresnel / 高光 / 透明度都取到正确值。
    bool underwater = !gl_FrontFacing;
    if (underwater) {
        N = -N;
    }

    float NoV = clamp(dot(N, V), 0.0, 1.0);

    // —— Fresnel（绝缘体 F0≈0.02）——
    const float F0 = 0.02;
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0);

    // —— 环境内容：水上=镜面反射 IBL，水下=透射天空（Snell 窗口）——
    // frame.iblParams.x > 0 表示环境图可用。水面以上取 reflect(-V,N) 的镜面反射；
    // 水面以下看不到"反射"，看到的是从上方透下来的光：按水(n≈1.33)→空气的
    // Snell 折射采样预滤波环境，超过临界角即全内反射（refract 返回 0）。
    vec3 reflCol = vec3(0.0);
    if (frame.iblParams.x > 0.0) {
        float roughLod = water.timeParams.z * frame.iblParams.x;
        if (underwater) {
            vec3 upN = normalize(geoN);
            vec3 airDir = refract(-V, -upN, 1.33);
            if (dot(airDir, airDir) > 1e-5) {
                // Snell 窗口边缘平滑衰减，避免与全内反射区硬切
                float cosCrit = 0.6614; // 临界角余弦 = 1/1.33 ≈ 48.6°
                float cosIn = clamp(dot(upN, -V), 0.0, 1.0);
                float window = smoothstep(cosCrit, 1.0, cosIn);
                reflCol = textureLod(samplerPrefilter, airDir, roughLod).rgb
                        * frame.iblParams.y * water.sizeParams.w
                        * kReflectionGain * window;
            }
        } else {
            vec3 R = reflect(-V, N);
            reflCol = textureLod(samplerPrefilter, R, roughLod).rgb
                    * frame.iblParams.y * water.sizeParams.w * kReflectionGain;
        }
    }

    // —— 方向光 / 点光 ——
    // 水几乎无朗伯漫反射：若把体色 ×(环境光/方向光) 当作受光底色，透明水面
    // 会被涂成不透明的染色玻璃。故水面受光只取 GGX 镜面高光；体色留待
    // 阶段 2 折射时以「深度吸收」方式出现，不从体色直接乘光照。
    vec3 L = normalize(-frame.dirLightDirection.xyz);
    float NdL = max(dot(N, L), 0.0);
    vec3 H = normalize(L + V);
    float D = distributionGGX(N, H, water.timeParams.z);
    float G = geometrySmith(N, V, L, water.timeParams.z);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), vec3(F0));
    vec3 specular = (D * G * F) / max(4.0 * NoV * NdL, 0.001);

    vec3 result = frame.dirLightColor.rgb * frame.dirLightColor.w * specular;

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
        result += pSpec * lc * atten;
    }

    // 环境内容按观察侧别混入：水面以上反射 ×Fresnel；水面以下透射天空
    // ×(1-Fresnel)（掠射角近全内反射 → 透射趋于 0，该处无上方光可显）。
    result += reflCol * (underwater ? (1.0 - fresnel) : fresnel);

    // 水面透明：Fresnel 提供视角轮廓（垂直较透、掠射较实），不透明度做主控。
    // 轮廓钳到 [0.12,1] 后抬升为 0.25+0.75×轮廓，让垂直视角也有基础实度，
    // 再乘整体不透明度（编辑器「不透明度」）——滑条 0→1 全角度线性响应。
    float fresnelProfile = clamp(fresnel + 0.1, 0.0, 1.0);
    float alpha;
    if (underwater) {
        // 水下表面保持半透明、透出背后内容：掠射方向若按 Fresnel 抬 alpha，
        // 而该处又没有透射天空（全内反射），会变成近黑的实心带。故水下统一
        // 低覆盖（随视角轻微加浓但仍远不到实心），让上方天空/场景透进来，
        // 只留一层淡淡的"水膜"质感。
        alpha = clamp(water.deepColor.a * (0.15 + 0.2 * fresnelProfile), 0.0, 1.0);
    } else {
        alpha = clamp(water.deepColor.a * (0.25 + 0.75 * fresnelProfile), 0.0, 1.0);
    }

    // 前向路径：ACES 色调映射后再输出（由 sRGB swapchain 硬件编码）
    outFragColor = vec4(acesToneMap(result), alpha);
}