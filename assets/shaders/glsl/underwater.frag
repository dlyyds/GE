#version 460

// UnderwaterFX stage-0 full-screen color pass.
// Reads Scene_HDR (RGBA16F, alpha = sky/geometry sentinel) + SceneDepth (R32),
// applies underwater media fog toward the submerged body deep color, plus mild
// desaturation/vignette scaled by the CPU submersion amount. alpha is passed
// through unchanged so Bloom/Tonemap keep sky-vs-geometry semantics intact.
layout (set = 0, binding = 0, std140) uniform UnderwaterUBO
{
    mat4 invProj;      // SceneDepth -> view-space reconstruction
    mat4 invView;      // view-space -> world-space reconstruction (segment B geometry gating)
    vec4 params;       // x=submersion(0..1), y=fogDensity(per meter), z=desaturate, w=vignette
    vec4 fogParams;    // x = FogDensity user multiplier (1.0 = current look)
    vec4 deepColor;    // underwater fog color (from the submerged body DeepColor)
    vec4 caustics;     // reserved (stage 2 segment B)
    vec4 sunDir;       // reserved (stage 2 segment B)
    vec4 waterPlane;   // x = planeY (underwater geometry gating, stage 2)
    vec4 timeParams;   // x = time, y = dt (reserved)
    vec4 viewPos;      // camera world position (reserved)
} waterFx;

layout (set = 0, binding = 1) uniform sampler2D samplerSceneColor;
layout (set = 0, binding = 2) uniform sampler2D samplerSceneDepth;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

// 段 B 程序化焦散：双层滚动正弦网叠加后锐化成“光斑”，无需资产。
float causticPattern(vec2 uv, float time)
{
    vec2 p1 = uv * 2.0 + vec2(time * 0.6, time * 0.35);
    vec2 p2 = uv * 3.0 + vec2(time * 0.45, time * 0.8);
    float g1 = 0.5 + 0.5 * sin(p1.x * 2.0 + sin(p1.y * 2.3) * 1.5);
    g1 *= 0.5 + 0.5 * sin(p1.y * 3.0 + cos(p1.x * 1.7) * 1.5);
    float g2 = 0.5 + 0.5 * sin(p2.x * 1.5 + sin(p2.y * 1.2) * 1.0);
    g2 *= 0.5 + 0.5 * sin(p2.y * 2.0 + cos(p2.x * 1.9) * 1.3);
    float c = mix(g1, g2, 0.5);
    return pow(max(c, 0.0), 3.0);
}

void main()
{
    vec4 scene = texture(samplerSceneColor, inUV);
    float sm = waterFx.params.x;
    if (sm <= 0.001)
    {
        outColor = vec4(scene.rgb, scene.a);
        return;
    }

    // Reconstruct view-space distance from the opaque scene depth snapshot.
    float rawDepth = texture(samplerSceneDepth, inUV).r;
    vec4 ndc = vec4(inUV * 2.0 - 1.0, rawDepth, 1.0);
    vec4 viewP = waterFx.invProj * ndc;
    vec3 viewPos3 = viewP.xyz / max(abs(viewP.w), 1e-6);
    float dist = max(-viewPos3.z, 0.0);

    // Depth fog toward underwater deep color, scaled by submersion.
    // Cap fog at 0.85 so geometry never fully blackens even at long depth.
    float fog = min(1.0 - exp(-waterFx.params.y * dist), 0.85);
    // FogDensity 用非线性幂控制可见度：<1 明显更亮，>1 更浓，1.0 完全保持现状。
    fog = pow(clamp(fog, 0.0, 1.0), 1.0 / max(waterFx.fogParams.x, 0.01));
    vec3 col = mix(scene.rgb, waterFx.deepColor.rgb, fog * sm);

    // Mild desaturation / vignette that deepen with submersion.
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(col, vec3(lum), waterFx.params.z * sm);
    col *= 1.0 - waterFx.params.w * sm * smoothstep(0.1, 0.9, length(inUV - 0.5));

    // —— 段 B 焦散（加性，只对水下几何生效）——
    // Scene_HDR.a 是天空/几何哨兵：天空/低 alpha 水面膜只做介质雾，不叠焦散。
    if (waterFx.caustics.z > 0.5 && scene.a >= 0.5) {
        vec4 worldP = waterFx.invView * vec4(viewPos3, 1.0);
        vec3 worldPos = worldP.xyz / max(abs(worldP.w), 1e-6);
        if (worldPos.y < waterFx.waterPlane.x - 0.05) {
            vec2 cUv = worldPos.xz * waterFx.caustics.y
                     - waterFx.sunDir.xz * waterFx.timeParams.x * 0.3;
            float c = causticPattern(cUv, waterFx.timeParams.x);
            col += c * waterFx.deepColor.rgb * waterFx.caustics.x * sm;
        }
    }

    // alpha must pass through unchanged (sky/geometry sentinel for downstream passes).
    outColor = vec4(col, scene.a);
}
