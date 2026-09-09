#version 460
// Stage 2: copy the opaque scene depth into an R32 sampled color resource (SceneDepth).
// The water Transparent pass uses it to reconstruct the water/scene depth difference.
layout (set = 0, binding = 0) uniform sampler2D samplerSource;
layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 outColor;
void main()
{
    float d = texture(samplerSource, inUV).r;
    outColor = vec4(d, d, d, 1.0);
}
