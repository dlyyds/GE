#version 460

// Bloom upsample: bilinearly upsample the smaller layer, then add a 9-tap
// Gaussian-weighted version of the same-level coarse layer. The wider, smooth
// 3x3 blur keeps the final glow temporally stable when the camera rotates.
layout (set = 0, binding = 0, std140) uniform BloomUBO
{
    vec4 params;      // reserved
    vec4 texelSize;   // x,y = 1 / current (output) size
} bloom;

layout (set = 0, binding = 1) uniform sampler2D samplerSmall;  // upsampled smaller layer
layout (set = 0, binding = 2) uniform sampler2D samplerCoarse; // same-level downsample result

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

void main()
{
    vec2 texel = bloom.texelSize.xy;

    // Bilinear resampling of the smaller layer approximates a broad glow.
    vec4 small = texture(samplerSmall, inUV);

    // 9-tap Gaussian soften of the coarse layer (weights 4:2:1).
    vec3 coarse =
          texture(samplerCoarse, inUV).rgb * 4.0
        + (texture(samplerCoarse, inUV + vec2( texel.x, 0.0)).rgb
         + texture(samplerCoarse, inUV + vec2(-texel.x, 0.0)).rgb
         + texture(samplerCoarse, inUV + vec2(0.0,  texel.y)).rgb
         + texture(samplerCoarse, inUV + vec2(0.0, -texel.y)).rgb) * 2.0
        + (texture(samplerCoarse, inUV + vec2( texel.x,  texel.y)).rgb
         + texture(samplerCoarse, inUV + vec2(-texel.x,  texel.y)).rgb
         + texture(samplerCoarse, inUV + vec2( texel.x, -texel.y)).rgb
         + texture(samplerCoarse, inUV + vec2(-texel.x, -texel.y)).rgb);

    coarse *= 1.0 / 16.0;

    outColor = vec4(small.rgb + coarse, 1.0);
}
