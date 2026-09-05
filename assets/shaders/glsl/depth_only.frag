#version 460

// 阴影专用片元着色器：只画深度，无颜色输出（阴影 pass 只有深度附件、零颜色附件）。
// 顶点级复用 mesh.vert / mesh_skinned.vert（输出 varyings 是超集，多出的忽略）。
// 只做 MASK 镂空裁剪：alpha 阈值判定与 mesh_gbuffer.frag 完全一致（Albedo alpha ×
// baseAlpha × tint），保证镂空植被的阴影空洞与可见轮廓对齐；Opaque 材质不采样。

layout (set = 1, binding = 0) uniform sampler2D samplerColor;

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
layout (location = 4) in flat vec4 inColor; // per-instance tint，与顶点着色器 outColor 对应

void main()
{
    // MASK（params.z = alphaCutoff >= 0）：镂空处丢弃 → 阴影图该处为空（光线穿透）。
    // Opaque（params.z < 0）：不采样不丢弃，整片元写深度。
    if (material.params.z >= 0.0) {
        float alpha = texture(samplerColor, inUV * material.params.w, 0.0).a
                      * material.emissiveFactor.w * inColor.a;
        if (alpha < material.params.z) discard;
    }
}
