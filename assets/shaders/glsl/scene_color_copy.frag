#version 460
// Stage 2: snapshot Lighting output Scene_HDR into Scene_HDR_Base.
// The water Transparent pass samples this instead of the live target to avoid self read/write.
layout (set = 0, binding = 0) uniform sampler2D samplerSource;
layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;
void main()
{
    outColor = texture(samplerSource, inUV);
}
