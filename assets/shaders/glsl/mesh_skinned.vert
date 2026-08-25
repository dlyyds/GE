#version 460

// 蒙皮顶点着色器：在 mesh.vert 基础上增加骨骼蒙皮变形。
// 顶点属性 location 必须与 C++ 端 Vertex 结构体顺序一致：
// Position → Normal → TexCoord → Tangent → JointIdx → Weight（共 80B）
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec4 inTangent;// xyz=切线方向，w=手性符号(+1/-1)
layout (location = 4) in uvec4 inJointIdx;// 影响该顶点的 4 个关节索引（0..N-1），见 A 阶段 Vertex 扩 80B
layout (location = 5) in vec4  inWeight;   // 与 inJointIdx 一一对应的权重（和≈1）

layout (set = 0, binding = 0, std140) uniform FrameUBO
{
    mat4 projection;
    mat4 view;
    vec4 viewPos;

// 方向光
    vec4 dirLightDirection;
    vec4 dirLightColor;

// 点光源数量（本体在 set 0 binding 1 的 SSBO 中，片元着色器使用）
    vec4 lightCount;// x = 点光源数量，yzw 填充对齐

// 环境光
    vec4 ambient;
} frame;

// 每个实例的数据：model（模型矩阵）+ color（叠加 tint）。
// 材质标量参数（如 shininess）已移到片元着色器的 per-material UBO（set 1 binding 2）。
struct InstanceData
{
    mat4 model;
    vec4 color;
};
layout (set = 2, binding = 0, std430) readonly buffer InstanceBuffer
{
    InstanceData instances[];
} instanceBuffer;

// 关节矩阵 SSBO（set 2 binding 1）：当前皮肤一根骨头一个 mat4。
// CPU 端按「一个皮肤 = 一段连续区间」单独绑定（C1 方案 ii），
// 绑定起始即该皮肤的第一块，故 BASE_JOINT 恒为 0；顶点内的关节索引是
// 皮肤内部索引（0..N-1），直接用它寻址即可，无需再加全局偏移。
layout (set = 2, binding = 1, std430) readonly buffer JointBuffer
{
    mat4 joints[];
} jointBuffer;
const int BASE_JOINT = 0;

layout (location = 0) out vec2 outUV;
layout (location = 1) out vec3 outWorldPos;
layout (location = 2) out vec3 outNormal;
layout (location = 3) out vec3 outViewVec;
layout (location = 4) out flat vec4 outColor;// per-instance tint，flat 不插值
// 法线贴图：世界空间切线 (T) 与副切线 (B)，片元着色器据此重建 TBN 矩阵
layout (location = 5) out vec3 outTangent;
layout (location = 6) out vec3 outBitangent;

void main()
{
    outUV = inUV;
    outColor = instanceBuffer.instances[gl_InstanceIndex].color;

    mat4 model = instanceBuffer.instances[gl_InstanceIndex].model;

    // —— 蒙皮：四关节加权合成一个蒙皮变换 ——
    // jointMatrix[j] = 关节 j 的当前世界矩阵 × 逆绑定矩阵（CPU 每帧算好上传）。
    // 权重各乘一块矩阵再相加，等价于对顶点的 4 个候选位置做加权平均。
    mat4 skin = inWeight.x * jointBuffer.joints[BASE_JOINT + inJointIdx.x]
              + inWeight.y * jointBuffer.joints[BASE_JOINT + inJointIdx.y]
              + inWeight.z * jointBuffer.joints[BASE_JOINT + inJointIdx.z]
              + inWeight.w * jointBuffer.joints[BASE_JOINT + inJointIdx.w];

    // 顶点先被蒙皮矩阵从绑定姿态挪到当前姿态，再走正常的 model → view → proj
    vec4 skinnedPos = skin * vec4(inPos, 1.0);
    vec4 worldPos = model * skinnedPos;
    gl_Position = frame.projection * frame.view * worldPos;

    outWorldPos = worldPos.xyz;

    // 法线/切线：先用蒙皮矩阵的 3×3 部分（忽略平移）随骨头弯曲，再经 model
    // 的法线矩阵（逆转置）保证非均匀缩放下仍垂直于表面。蒙皮矩阵可能引入
    // 非均匀缩放（两骨头加权混合），故先归一化再走法线矩阵。
    mat3 skinNormal = mat3(skin);
    mat3 normalMatrix = mat3(inverse(transpose(model)));
    outNormal   = normalMatrix * normalize(skinNormal * inNormal);
    outTangent  = normalMatrix * normalize(skinNormal * inTangent.xyz);
    // 副切线 = 法线 × 切线，再乘手性符号恢复正确的左右手系
    outBitangent = cross(outNormal, outTangent) * inTangent.w;

    outViewVec = frame.viewPos.xyz - worldPos.xyz;
}