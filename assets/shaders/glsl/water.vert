#version 460
/* —— 水面顶点着色器（Gerstner 波）——
 * 输入为 MeshManager::GetWaterGrid 生成的 XZ 平面细分网格。
 * 在顶点阶段累加最多 4 层 Gerstner 波位移，并用解析偏导重建法线。
 */

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent; // 未用于水面（法线由波公式重建）

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

layout(set = 0, binding = 2, std140) uniform WaterUBO
{
    mat4 model;
    vec4 timeParams;    // x=time, y=normalStrength, z=roughness, w=refractionStrength
    vec4 deepColor;
    vec4 shallowColor;
    vec4 sizeParams;    // x=size.x, y=size.y, z=normalTiling, w=reflectionStrength
    vec4 foamParams;    // x=foamDistance, y=foamIntensity, z=absorptionDepth, w=timeScale
    vec4 waves[4];      // x=dirX, y=dirY, z=amplitude, w=wavelength
    vec4 waveSpeeds[4]; // x=speed
    vec4 colorParams;   // 与片元一致（顶点阶段未使用，仅保持布局相同）
    mat4 invProj;     // inverse projection for SceneDepth reconstruction
} water;

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec3 outWorldPos;
layout(location = 2) out vec3 outNormal;

const float PI = 3.14159265359;

void main()
{
    vec3 p = (water.model * vec4(inPosition, 1.0)).xyz;

    // 初始斜率：未扰动平面 dP/dx=(1,0,0), dP/dz=(0,0,1)
    vec3 dxAcc = vec3(1.0, 0.0, 0.0);
    vec3 dzAcc = vec3(0.0, 0.0, 1.0);

    for (int i = 0; i < 4; ++i) {
        vec4 wave = water.waves[i];
        float wavelength = wave.w;
        if (wavelength <= 0.0) {
            continue; // 波长 0 = 未启用
        }

        float speed = water.waveSpeeds[i].x;
        float k = 2.0 * PI / wavelength;
        float angular = k * speed;
        float steepness = 1.0; // 暂时固定，后续可由组件参数扩展
        float qk = steepness * wave.z;

        vec2 D = normalize(wave.xy);
        float f = k * dot(D, p.xz) - angular * water.timeParams.x;
        float cosF = cos(f);
        float sinF = sin(f);

        p.x += qk * D.x * cosF;
        p.z += qk * D.y * cosF;
        p.y -= wave.z * sinF;

        dxAcc.x += -qk * D.x * D.x * k * sinF;
        dxAcc.y += -wave.z * D.x * k * cosF;
        dxAcc.z += -qk * D.x * D.y * k * sinF;

        dzAcc.x += -qk * D.x * D.y * k * sinF;
        dzAcc.y += -wave.z * D.y * k * cosF;
        dzAcc.z += -qk * D.y * D.y * k * sinF;
    }

    gl_Position = frame.projection * frame.view * vec4(p, 1.0);
    outUV = inUV;
    outWorldPos = p;
    outNormal = normalize(cross(dzAcc, dxAcc));
}