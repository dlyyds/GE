#version 450

layout(binding = 0, std140) uniform UniformBlock {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 color;
} u_Block;

layout(binding = 1) uniform sampler2D u_Texture;

layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 out_color;

void main()
{
    vec4 tex_color = texture(u_Texture, v_uv);
    out_color = tex_color;
}
