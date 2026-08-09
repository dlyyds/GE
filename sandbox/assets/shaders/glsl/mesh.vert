#version 460
/* Copyright (c) 2019-2024, Sascha Willems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// 顶点属性 location 必须与 C++ 端 Vertex 结构体的内存顺序一致：
// Position → Normal → TexCoord → Tangent
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec4 inTangent;// xyz=切线方向，w=手性符号(+1/-1)

// 点光源最大数量，必须与 C++ 端 Renderer3D::MAX_POINT_LIGHTS 保持一致
#define MAX_POINT_LIGHTS 8

layout (set = 0, binding = 0, std140) uniform FrameUBO
{
    mat4 projection;
    mat4 view;
    vec4 viewPos;

// 方向光
    vec4 dirLightDirection;
    vec4 dirLightColor;

// 点光源数组（每个灯 2 个 vec4：position.w = 半径倒数，color.a = 强度）
    vec4 pointLightPositions[MAX_POINT_LIGHTS];
    vec4 pointLightColors[MAX_POINT_LIGHTS];
    vec4 pointLightCount;// x = 实际点光源数量，yzw 填充对齐

// 环境光
    vec4 ambient;
} frame;

struct InstanceData
{
    mat4 model;
    vec4 color;
    float shininess;
};
layout (set = 2, binding = 0, std430) readonly buffer InstanceBuffer
{
    InstanceData instances[];
} instanceBuffer;

layout (location = 0) out vec2 outUV;
layout (location = 1) out vec3 outWorldPos;
layout (location = 2) out vec3 outNormal;
layout (location = 3) out vec3 outViewVec;
layout (location = 4) out flat vec4 outColor;// per-instance tint，flat 不插值
// 法线贴图：世界空间切线 (T) 与副切线 (B)，片元着色器据此重建 TBN 矩阵
layout (location = 5) out vec3 outTangent;
layout (location = 6) out vec3 outBitangent;
layout (location = 7) out flat float outShininess;// 材质高光指数（per-instance，flat 不插值）

void main()
{
    outUV = inUV;
    outColor = instanceBuffer.instances[gl_InstanceIndex].color;
    outShininess = instanceBuffer.instances[gl_InstanceIndex].shininess;

    mat4 model = instanceBuffer.instances[gl_InstanceIndex].model;
    vec4 worldPos = model * vec4(inPos, 1.0);
    gl_Position = frame.projection * frame.view * worldPos;

    outWorldPos = worldPos.xyz;

    // 法线与切线都经法线矩阵（逆转置）变换，保证非均匀缩放下仍垂直于表面
    mat3 normalMatrix = mat3(inverse(transpose(model)));
    outNormal   = normalMatrix * inNormal;
    outTangent  = normalMatrix * inTangent.xyz;
    // 副切线 = 法线 × 切线，再乘手性符号恢复正确的左右手系
    outBitangent = cross(outNormal, outTangent) * inTangent.w;

    outViewVec = frame.viewPos.xyz - worldPos.xyz;
}