#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;

layout(binding = 0, std140) uniform UniformBlock {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 color;
} u_Block;

layout(location = 0) out vec2 v_uv;

void main()
{
    gl_Position = u_Block.projection * u_Block.view * u_Block.model * vec4(in_position, 0.0, 1.0);
    v_uv = in_uv;
}
