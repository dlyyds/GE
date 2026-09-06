#version 460

// Bloom 升采样/模糊片元：把低分辨率层升回当前 mip，并与同尺寸粗层相加软化。
layout (set = 0, binding = 0, std140) uniform BloomUBO
{
    vec4 params;      // 预留
    vec4 texelSize;   // x,y = 1 / 当前（输出）尺寸
} bloom;

layout (set = 0, binding = 1) uniform sampler2D samplerSmall;  // 上一层小纹理（硬件已升采样到近似当前尺寸）
layout (set = 0, binding = 2) uniform sampler2D samplerCoarse; // 当前层的降采样结果

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

void main()
{
    vec2 texel = bloom.texelSize.xy;

    // 双线性采样小纹理（近似上一级光晕）
    vec4 small = texture(samplerSmall, inUV);

    // 当前层粗结果做 4-tap 软化，避免边缘硬切
    vec4 coarse =
          (texture(samplerCoarse, inUV + vec2(-texel.x, 0.0))
         + texture(samplerCoarse, inUV + vec2( texel.x, 0.0))
         + texture(samplerCoarse, inUV + vec2(0.0, -texel.y))
         + texture(samplerCoarse, inUV + vec2(0.0,  texel.y))) * 0.25;

    outColor = small + coarse;
}
