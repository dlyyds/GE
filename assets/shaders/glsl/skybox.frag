#version 460

// 天空盒片元着色器：cubemap 采样。
//
// 核心思路：全屏像素的 UV 反投影重建视线方向，直接从 cubemap 按方向采样。
//   1. NDC（inUV*2-1）→ 视空间视线：乘以 invProj（投影矩阵逆）
//   2. 视空间 → 世界空间方向：乘以仅旋转的视图矩阵逆（mat3(invView)，去掉平移，
//      使天空盒不受相机位置影响，始终显得"无限远"）
//   3. 世界方向直接采样 samplerCube
//
// 说明：直接输出采样值，不做 tonemap。环境 cubemap 数据（R16G16B16A16_SFLOAT）
// 的亮度范围已适合显示，渲染目标若为 sRGB 会自动处理 gamma，直接采样输出
// 即可得到与参考（Blender）一致的外观。加 ACES/曝光/饱和度反而会漂白发灰。

layout (set = 0, binding = 0, std140) uniform SkyboxUBO
{
    mat4 invView;// 仅旋转部分视图矩阵的逆（世界方向 ← 视方向）
    mat4 invProj;// 投影矩阵的逆（视空间视线 ← NDC 坐标）
} ubo;

layout (set = 0, binding = 1) uniform samplerCube uSkybox;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

void main()
{
    // NDC → 视空间视线（vec4 透视除法前先反投影）
    vec4 viewRay = ubo.invProj * vec4(inUV * 2.0 - 1.0, 1.0, 1.0);
    vec3 dir = normalize(viewRay.xyz / viewRay.w);

    // 视空间 → 世界空间方向（mat3 取旋转部分，天空盒不随相机位移）
    vec3 worldDir = normalize(mat3(ubo.invView) * dir);

    // 直接按方向采样 cubemap 并输出
    vec3 color = texture(uSkybox, worldDir).rgb;
    outColor = vec4(color, 1.0);
}