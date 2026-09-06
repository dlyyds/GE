#version 460

// Bloom 降采样片元：2x2 box 平均，把上一级高光层再缩一半。
layout (set = 0, binding = 0, std140) uniform BloomUBO
{
    vec4 params;      // 预留
    vec4 texelSize;   // x,y = 1 / 上一级（源）尺寸
} bloom;

layout (set = 0, binding = 1) uniform sampler2D samplerScene;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

void main()
{
    vec2 texel = bloom.texelSize.xy;

    // 四角取 2x2 box 平均，从源层降采样到当前层。
    vec4 result =
          texture(samplerScene, inUV + vec2(-texel.x, -texel.y) * 0.5)
        + texture(samplerScene, inUV + vec2( texel.x, -texel.y) * 0.5)
        + texture(samplerScene, inUV + vec2(-texel.x,  texel.y) * 0.5)
        + texture(samplerScene, inUV + vec2( texel.x,  texel.y) * 0.5);

    outColor = result * 0.25;
}
