#version 460

// 延迟 Lighting 片元着色器：逐像素读 GBuffer 做 PBR / Blinn-Phong 光照，
// 并把天空盒背景并入此 pass。G0.a 为着色模型哨兵，0 表示天空像素。

layout (set = 0, binding = 0, std140) uniform LightingUBO
{
    mat4 invView;
    mat4 invProj;
    vec4 clearColor;
    vec4 flags;
    vec4 viewPos;
    vec4 dirLightDirection;
    vec4 dirLightColor;
    vec4 lightCount;
    vec4 ambient;
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
    vec3 ambientColor = lighting.ambient.rgb * lighting.ambient.w;
    vec3 result = ambientColor * albedo;

    if (modelFlag < kBlinnMiddle) {
        float shininess = scalarA;
        float specularStrength = scalarB;
        result += calcDirectionalLightBlinn(N, V, albedo, shininess, specularStrength);
        for (int i = 0; i < int(lighting.lightCount.x); i++) {
            result += calcPointLightBlinn(lightBuffer.lights[i], N, V, worldPos,
                                          albedo, shininess, specularStrength);
        }
    } else {
        float roughness = scalarA;
        float metallic = scalarB;
        result += calcDirectionalLightPBR(N, V, albedo, metallic, roughness);
        for (int i = 0; i < int(lighting.lightCount.x); i++) {
            result += calcPointLightPBR(lightBuffer.lights[i], N, V, worldPos,
                                        albedo, metallic, roughness);
        }
    }

    result += emissive;
    outColor = vec4(acesToneMap(result), 1.0);
}
