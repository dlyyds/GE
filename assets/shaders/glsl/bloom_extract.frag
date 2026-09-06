#version 460

// Bloom 阈值提取片元：从 Scene_HDR 中抽取「几何/合成」像素的高光。
// 天空像素（alpha<0.5）不参与 bloom，直接输出黑，避免环境图被错误放大。
layout (set = 0, binding = 0, std140) uniform BloomUBO
{
    vec4 params;      // x = threshold，y = intensity（合成时用），z = 启用，w 预留
    vec4 texelSize;   // x,y = 1 / 当前 mip 尺寸，zw 预留
} bloom;

layout (set = 0, binding = 1) uniform sampler2D samplerHDR;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;

void main()
{
    vec4 hdr = texture(samplerHDR, inUV);

    // 天空 / 未合成区域不参与 bloom
    if (hdr.a < 0.5)
    {
        outColor = vec4(0.0);
        return;
    }

    // 保留 HDR 颜色而非亮度，保证有色高光泛光不偏灰。
    vec3 bright = max(hdr.rgb - bloom.params.x, vec3(0.0));
    outColor = vec4(bright, 1.0);
}
