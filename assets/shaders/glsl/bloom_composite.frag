#version 460

// Bloom 合成片元：把最终泛光加回 Scene_HDR，保持 alpha 元数据（天空=0 / 几何=1）。
layout (set = 0, binding = 0, std140) uniform BloomUBO
{
    vec4 params;      // x = threshold，y = intensity，z = 启用，w 预留
    vec4 texelSize;   // 预留
} bloom;

layout (set = 0, binding = 1) uniform sampler2D samplerHDR;
layout (set = 0, binding = 2) uniform sampler2D samplerBloom;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

void main()
{
    vec4 hdr = texture(samplerHDR, inUV);
    vec3 glow = texture(samplerBloom, inUV).rgb * bloom.params.y;

    // 只给几何像素加泛光；天空像素 alpha=0 不叠加。
    vec3 color = hdr.rgb + glow * step(0.5, hdr.a);

    outColor = vec4(color, hdr.a);
}
