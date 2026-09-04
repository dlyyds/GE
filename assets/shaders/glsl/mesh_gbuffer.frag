#version 460

// GBuffer 片元着色器：把材质属性写入 4 张 MRT 附件，光照留在 Lighting pass。
// 顶点级复用 mesh.vert / mesh_skinned.vert，因此 set 布局与输入 location 与
// 现有前向片元一致（FrameUBO + InstanceData + texture/material 槽位）。

layout (set = 1, binding = 0) uniform sampler2D samplerColor;
layout (set = 1, binding = 1) uniform sampler2D samplerNormal;
layout (set = 1, binding = 3) uniform sampler2D samplerEmissive;
layout (set = 1, binding = 4) uniform sampler2D samplerMetallicRoughness;

layout (set = 0, binding = 0, std140) uniform FrameUBO
{
    mat4 projection;
    mat4 view;
    vec4 viewPos;
    vec4 dirLightDirection;
    vec4 dirLightColor;
    vec4 lightCount;
    vec4 ambient;
} frame;

layout (set = 1, binding = 2, std140) uniform MaterialUBO
{
    vec4 params;
    vec4 pbr;
    vec4 emissiveFactor;
} material;

layout (location = 0) in vec2 inUV;
layout (location = 1) in vec3 inWorldPos;
layout (location = 2) in vec3 inNormal;
layout (location = 3) in vec3 inViewVec;
layout (location = 4) in flat vec4 inColor;
layout (location = 5) in vec3 inTangent;
layout (location = 6) in vec3 inBitangent;

layout (location = 0) out vec4 outG0;
layout (location = 1) out vec4 outG1;
layout (location = 2) out vec4 outG2;
layout (location = 3) out vec4 outG3;

const float kBlinnFlag = 1.0 / 255.0;
const float kPbrFlag   = 2.0 / 255.0;

void main()
{
    vec4 texColor = texture(samplerColor, inUV * material.params.w, 0.0);
    vec3 albedo = texColor.rgb * inColor.rgb;

    float alpha = texColor.a * material.emissiveFactor.w * inColor.a;
    if (material.params.z >= 0.0 && alpha < material.params.z) discard;

    vec3 tangentNormal = texture(samplerNormal, inUV * material.params.w, 0.0).rgb * 2.0 - 1.0;
    vec3 T = normalize(inTangent);
    vec3 B = normalize(inBitangent);
    vec3 N = normalize(mat3(T, B, normalize(inNormal)) * tangentNormal);

    vec3 emissive = texture(samplerEmissive, inUV * material.params.w, 0.0).rgb
                    * material.emissiveFactor.rgb;

    const float modelFlag = material.pbr.z > 0.5 ? kPbrFlag : kBlinnFlag;
    float scalarA = 0.0;
    float scalarB = 0.0;
    if (material.pbr.z > 0.5) {
        // PBR：roughness / metallic。
        vec4 mr = texture(samplerMetallicRoughness, inUV * material.params.w, 0.0);
        scalarA = mr.g * material.pbr.y;
        scalarB = mr.b * material.pbr.x;
    } else {
        // Blinn-Phong：shininess / specularStrength。
        scalarA = material.params.x;
        scalarB = material.params.y;
    }

    outG0 = vec4(albedo, modelFlag);
    outG1 = vec4(N, 1.0);
    outG2 = vec4(inWorldPos, scalarA);
    outG3 = vec4(emissive, scalarB);
}
