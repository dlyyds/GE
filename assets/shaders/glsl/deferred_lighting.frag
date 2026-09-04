#version 460

// 延迟 Lighting 片元着色器：逐像素读 GBuffer 做 PBR / Blinn-Phong 光照，
// 并把天空盒背景并入此 pass。G0.a 为着色模型哨兵，0 表示天空像素。
// PBR 环境光支持 split-sum IBL（flags.y 门控），未就绪时回退常量环境光。

layout (set = 0, binding = 0, std140) uniform LightingUBO
{
    mat4 invView;
    mat4 invProj;
    vec4 clearColor;
    vec4 flags;        // x = 天空盒开关，y = IBL 开关，zw 预留
    vec4 viewPos;
    vec4 dirLightDirection;
    vec4 dirLightColor;
    vec4 lightCount;
    vec4 ambient;
    vec4 iblParams;    // x = 预滤波最大 mip 数（MAX_REFLECTION_LOD），yzw 预留
} lighting;

struct PointLight
{
    vec4 position;
    vec4 color;
};
layout (set = 0, binding = 1, std430) readonly buffer LightBuffer
{
    PointLight lights[];
} lightBuffer;

layout (set = 0, binding = 2) uniform samplerCube samplerSkybox;

layout (set = 1, binding = 0) uniform sampler2D samplerG0;
layout (set = 1, binding = 1) uniform sampler2D samplerG1;
layout (set = 1, binding = 2) uniform sampler2D samplerG2;
layout (set = 1, binding = 3) uniform sampler2D samplerG3;

// —— IBL 环境光三件套（绑定布局与 mesh_pbr_ibl.frag 对齐，追加在 G0~G3 之后）——
// 辐照度采样器复用预滤波 cubemap：漫反射取最高 mip（近似余弦卷积），
// 镜面按粗糙度取 mip。BRDF LUT 是 2D split-sum 表，轴 (NoV, roughness)。
// flags.y = 0 时走常量环境光回退，本分支不采样这三张。
layout (set = 1, binding = 4) uniform samplerCube samplerIrradiance;// 漫反射辐照度（= 预滤波图最高 mip）
layout (set = 1, binding = 5) uniform samplerCube samplerPrefilter; // 镜面预滤波 mip 链 cubemap
layout (set = 1, binding = 6) uniform sampler2D  samplerBrdfDFG;    // BRDF LUT（2D，(NoV, roughness)）

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

const float PI = 3.14159265359;
const float kSkyThreshold = 0.5 / 255.0;
const float kBlinnMiddle = 1.5 / 255.0;

vec3 acesToneMap(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) /
                 (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return geometrySchlickGGX(max(dot(N, V), 0.0), roughness)
    * geometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 calcDirectLight(vec3 N, vec3 V, vec3 L, vec3 radiance,
                     vec3 albedo, float metallic, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F  = fresnelSchlick(max(dot(H, V), 0.0), F0);

    float NDF = distributionGGX(N, H, roughness);
    float G   = geometrySmith(N, V, L, roughness);

    vec3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, 0.001);

    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    return (kD * albedo / PI + specular) * radiance * NdotL;
}

vec3 calcDirectionalLightPBR(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness) {
    vec3 L = normalize(-lighting.dirLightDirection.xyz);
    vec3 radiance = lighting.dirLightColor.rgb * lighting.dirLightColor.w;
    return calcDirectLight(N, V, L, radiance, albedo, metallic, roughness);
}

vec3 calcPointLightPBR(PointLight light, vec3 N, vec3 V, vec3 worldPos,
                       vec3 albedo, float metallic, float roughness) {
    vec3 L = light.position.xyz - worldPos;
    float dist = length(L);
    L = normalize(L);
    float attenuation = 1.0 / (1.0 + dist * dist * light.position.w * light.position.w);
    vec3 radiance = light.color.rgb * light.color.a * attenuation;
    return calcDirectLight(N, V, L, radiance, albedo, metallic, roughness);
}

vec3 calcDirectionalLightBlinn(vec3 N, vec3 V, vec3 albedo,
                               float shininess, float specularStrength) {
    vec3 L = normalize(-lighting.dirLightDirection.xyz);
    vec3 lightColor = lighting.dirLightColor.rgb * lighting.dirLightColor.w;
    float diff = max(dot(N, L), 0.0);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), shininess);
    return diff * lightColor * albedo + spec * lightColor * specularStrength;
}

vec3 calcPointLightBlinn(PointLight light, vec3 N, vec3 V, vec3 worldPos, vec3 albedo,
                         float shininess, float specularStrength) {
    vec3 L = light.position.xyz - worldPos;
    float dist = length(L);
    L = normalize(L);
    float attenuation = 1.0 / (1.0 + dist * dist * light.position.w * light.position.w);
    vec3 lightColor = light.color.rgb * light.color.a;
    float diff = max(dot(N, L), 0.0);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), shininess);
    return (diff * lightColor * albedo + spec * lightColor * specularStrength) * attenuation;
}

// PBR 环境光：flags.y 开启时走 split-sum IBL（与 mesh_pbr_ibl.frag 同公式：
// 粗糙度感知菲涅尔 + 辐照度漫反射 + 预滤波镜面 + BRDF LUT），
// 否则回退常量环境光。Blinn-Phong 不参与 IBL，始终用常量环境光。
vec3 calcAmbientPBR(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness) {
    if (lighting.flags.y > 0.5) {
        vec3 F0 = mix(vec3(0.04), albedo, metallic);
        float NoV = max(dot(N, V), 0.0);

        vec3 F = F0 + (max(vec3(1.0 - roughness), F0) - F0)
                * pow(clamp(1.0 - NoV, 0.0, 1.0), 5.0);
        vec3 kS = F;
        vec3 kD = (1.0 - kS) * (1.0 - metallic);

        vec3 irradiance = textureLod(samplerIrradiance, N, lighting.iblParams.x).rgb;
        vec3 diffuse = irradiance * albedo * kD;

        vec3 R = reflect(-V, N);
        vec3 prefiltered = textureLod(samplerPrefilter, R,
                                      roughness * lighting.iblParams.x).rgb;
        vec2 brdf = texture(samplerBrdfDFG, vec2(NoV, roughness)).rg;
        vec3 specular = prefiltered * (F * brdf.r + brdf.g);

        return diffuse + specular;
    }

    vec3 ambientColor = lighting.ambient.rgb * lighting.ambient.w;
    return ambientColor * albedo;
}

void main()
{
    vec3 g0 = texture(samplerG0, inUV).rgb;
    float modelFlag = texture(samplerG0, inUV).a;

    if (modelFlag < kSkyThreshold) {
        vec4 viewRay = lighting.invProj * vec4(inUV * 2.0 - 1.0, 1.0, 1.0);
        vec3 dir = normalize(viewRay.xyz / viewRay.w);
        vec3 worldDir = normalize(mat3(lighting.invView) * dir);

        vec3 color = lighting.clearColor.rgb;
        if (lighting.flags.x > 0.5) {
            color = texture(samplerSkybox, worldDir).rgb;
        }
        outColor = vec4(color, 1.0);
        return;
    }

    vec3 albedo = g0;
    vec3 N = normalize(texture(samplerG1, inUV).xyz);
    vec3 worldPos = texture(samplerG2, inUV).xyz;
    float scalarA = texture(samplerG2, inUV).a;
    vec4 g3 = texture(samplerG3, inUV);
    vec3 emissive = g3.rgb;
    float scalarB = g3.a;

    vec3 V = normalize(lighting.viewPos.xyz - worldPos);

    vec3 result;
    if (modelFlag < kBlinnMiddle) {
        float shininess = scalarA;
        float specularStrength = scalarB;
        vec3 ambientColor = lighting.ambient.rgb * lighting.ambient.w;
        result = ambientColor * albedo;
        result += calcDirectionalLightBlinn(N, V, albedo, shininess, specularStrength);
        for (int i = 0; i < int(lighting.lightCount.x); i++) {
            result += calcPointLightBlinn(lightBuffer.lights[i], N, V, worldPos,
                                          albedo, shininess, specularStrength);
        }
    } else {
        float roughness = scalarA;
        float metallic = scalarB;
        result = calcAmbientPBR(N, V, albedo, metallic, roughness);
        result += calcDirectionalLightPBR(N, V, albedo, metallic, roughness);
        for (int i = 0; i < int(lighting.lightCount.x); i++) {
            result += calcPointLightPBR(lightBuffer.lights[i], N, V, worldPos,
                                        albedo, metallic, roughness);
        }
    }

    result += emissive;
    outColor = vec4(acesToneMap(result), 1.0);
}
