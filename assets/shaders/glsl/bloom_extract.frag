#version 460

// Bloom threshold extraction: emit bright pixels from the full-res Scene_HDR,
// excluding sky. Thresholding happens BEFORE any mip downsample, so a sub-pixel
// metallic glint is first captured as a full-res bright texel and then smoothed
// by the Karis-average downsample that follows. This avoids the flicker caused by
// pre-filter averaging a small glint below the threshold before it ever blooms.
layout (set = 0, binding = 0, std140) uniform BloomUBO
{
    vec4 params;      // x = threshold, y = intensity (composite only), z = enabled, w reserved
    vec4 texelSize;   // reserved (full-res extraction uses the source extent only)
} bloom;

layout (set = 0, binding = 1) uniform sampler2D samplerHDR;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

void main()
{
    vec4 hdr = texture(samplerHDR, inUV);

    // Sky / uncomposited pixels never feed bloom.
    if (hdr.a < 0.5)
    {
        outColor = vec4(0.0);
        return;
    }

    // Keep HDR color rather than luminance so chromatic highlights stay tinted.
    vec3 bright = max(hdr.rgb - bloom.params.x, vec3(0.0));

    // Soft knee: fade in across [threshold, threshold + kneeWidth]. Combined with
    // the later Karis downsample this keeps a glint smooth as it crosses nearby
    // pixels, instead of hard popping at the threshold.
    vec3 kneeWidth = vec3(max(bloom.params.x * 0.25, 0.05));
    vec3 knee = smoothstep(vec3(0.0), kneeWidth, bright);

    outColor = vec4(bright * knee, 1.0);
}
