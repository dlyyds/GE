#version 460

// Bloom downsample: 2x2 weighted average (Karis average) that halves the
// previous mip. texel offsets are computed from the source textureSize so the
// 2x2 taps cover exactly one source pixel per sample, and bright "fireflies"
// are down-weighted to reduce flicker during camera movement.
layout (set = 0, binding = 0, std140) uniform BloomUBO
{
    vec4 params;      // reserved
    vec4 texelSize;   // reserved (this pass uses textureSize for source extent)
} bloom;

layout (set = 0, binding = 1) uniform sampler2D samplerScene;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

float luminanceWeight(vec3 c)
{
    return 1.0 / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722)));
}

void main()
{
    // Use the SOURCE mip extent, not the output (half) extent, for correct taps.
    ivec2 srcSize = textureSize(samplerScene, 0);
    vec2 texel = 1.0 / vec2(max(srcSize.x, 1), max(srcSize.y, 1));

    vec3 c0 = texture(samplerScene, inUV + vec2(-texel.x, -texel.y) * 0.5).rgb;
    vec3 c1 = texture(samplerScene, inUV + vec2( texel.x, -texel.y) * 0.5).rgb;
    vec3 c2 = texture(samplerScene, inUV + vec2(-texel.x,  texel.y) * 0.5).rgb;
    vec3 c3 = texture(samplerScene, inUV + vec2( texel.x,  texel.y) * 0.5).rgb;

    // Karis average: give very bright taps less weight so one firefly cannot
    // pop in and out of the downsampled bloom mips.
    float w0 = luminanceWeight(c0);
    float w1 = luminanceWeight(c1);
    float w2 = luminanceWeight(c2);
    float w3 = luminanceWeight(c3);
    float wSum = w0 + w1 + w2 + w3;

    outColor = vec4((c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3) / max(wSum, 1e-6), 1.0);
}
