#version 460

// 天空盒片元着色器：等距柱状投影（equirectangular）采样。
//
// 核心思路：全屏像素的 UV 反投影重建视线方向，再从全景图采样该方向颜色。
//   1. NDC（inUV*2-1）→ 视空间视线：乘以 invProj（投影矩阵逆）
//   2. 视空间 → 世界空间方向：乘以仅旋转的视图矩阵逆（mat3(invView)，去掉平移，
//      使天空盒不受相机位置影响，始终显得"无限远"）
//   3. 世界方向 → 等距 UV：phi = atan(x,z)，theta = acos(y)，再归一化到 [0,1]
//
// 说明：等距图画面上方对应天空（theta=0），下方对应地面（theta=PI），
// worldDir.y 向上，故此映射与 HDRI 全景图一致。

layout (set = 0, binding = 0, std140) uniform SkyboxUBO
{
    mat4 invView;   // 仅旋转部分视图矩阵的逆（世界方向 ← 视方向）
    mat4 invProj;   // 投影矩阵的逆（视空间视线 ← NDC 坐标）
} ubo;

layout (set = 0, binding = 1) uniform sampler2D uEquirect;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

const float PI = 3.14159265359;

void main()
{
    // NDC → 视空间视线（vec4 透视除法前先反投影）
    vec4 viewRay = ubo.invProj * vec4(inUV * 2.0 - 1.0, 1.0, 1.0);
    vec3 dir = normalize(viewRay.xyz / viewRay.w);

    // 视空间 → 世界空间方向（mat3 取旋转部分，天空盒不随相机位移）
    vec3 worldDir = normalize(mat3(ubo.invView) * dir);

    // 等距柱状映射：方向 → UV
    //   phi   = atan(x, z)：-PI..PI，经度
    //   theta = acos(y)：0..PI，纬度（0=正上，PI=正下）
    vec2 uv = vec2(atan(worldDir.z, worldDir.x) / (2.0 * PI) + 0.5,
                   acos(clamp(worldDir.y, -1.0, 1.0)) / PI);

    outColor = vec4(texture(uEquirect, uv).rgb, 1.0);
}