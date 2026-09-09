#version 460

// UnderwaterFX stage-0 full-screen color pass.
// Reads Scene_HDR (RGBA16F, alpha = sky/geometry sentinel) + SceneDepth (R32),
// applies underwater media fog toward the submerged body deep color, plus mild
// desaturation/vignette scaled by the CPU submersion amount. alpha is passed
// through unchanged so Bloom/Tonemap keep sky-vs-geometry semantics intact.
layout (set = 0, binding = 0, std140) uniform UnderwaterUBO
{
    mat4 invProj;      // SceneDepth -> view-space reconstruction
    vec4 params;       // x=submersion(0..1), y=fogDensity(per meter), z=desaturate, w=vignette
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
    vec3 col = mix(scene.rgb, waterFx.deepColor.rgb, fog * sm);

    // Mild desaturation / vignette that deepen with submersion.
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(col, vec3(lum), waterFx.params.z * sm);
    col *= 1.0 - waterFx.params.w * sm * smoothstep(0.1, 0.9, length(inUV - 0.5));

    // alpha must pass through unchanged (sky/geometry sentinel for downstream passes).
    outColor = vec4(col, scene.a);
}
