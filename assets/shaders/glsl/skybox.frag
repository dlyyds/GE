#version 460

// 天空盒片元着色器：cubemap 采样。
//
// 核心思路：全屏像素的 UV 反投影重建视线方向，直接从 cubemap 按方向采样。
//   1. NDC（inUV*2-1）→ 视空间视线：乘以 invProj（投影矩阵逆）
//   2. 视空间 → 世界空间方向：乘以仅旋转的视图矩阵逆（mat3(invView)，去掉平移，
//      使天空盒不受相机位置影响，始终显得"无限远"）
//   3. 世界方向直接采样 samplerCube
//
// 采用 HDR 天空盒（R16G16B16A16_SFLOAT），因此采样后需做 tonemap + gamma，
// 否则过曝。

layout (set = 0, binding = 0, std140) uniform SkyboxUBO
{
    mat4 invView;   // 仅旋转部分视图矩阵的逆（世界方向 ← 视方向）
    mat4 invProj;   // 投影矩阵的逆（视空间视线 ← NDC 坐标）
} ubo;

layout (set = 0, binding = 1) uniform samplerCube uSkybox;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

// 曝光参数：把天空抬到舒适亮度，同时避免过度漂白。
// 该环境图天空大部分在 0.1~0.9（P50≈0.26），太阳高达 5.7e4。
const float kExposure = 1.5;

// ACES filmic tonemap（Narkowicz 近似）：高对比，接近 Blender 的 Filmic。
vec3 acesFilmic(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main()
{
    // NDC → 视空间视线（vec4 透视除法前先反投影）
    vec4 viewRay = ubo.invProj * vec4(inUV * 2.0 - 1.0, 1.0, 1.0);
    vec3 dir = normalize(viewRay.xyz / viewRay.w);

    // 视空间 → 世界空间方向（mat3 取旋转部分，天空盒不随相机位移）
    vec3 worldDir = normalize(mat3(ubo.invView) * dir);

    // 直接按方向采样 cubemap
    vec3 color = texture(uSkybox, worldDir).rgb;

    // 曝光 → HDR tonemap（ACES filmic）→ gamma 校正
    color *= kExposure;
    color = acesFilmic(color);
    color = pow(color, vec3(1.0 / 2.2));

    // 饱和度恢复：ACES/tonemap 压缩动态范围时会降低饱和度（漂白成灰），
    // 用 luma 插值补回饱和度，让蓝色天空保持蓝（mix 系数 >1 提饱和）。
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, 1.25);

    outColor = vec4(color, 1.0);
}