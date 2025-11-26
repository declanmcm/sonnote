#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(binding = 0) uniform Ubo {
    mat4 projection; // Orthographic projection matrix
} ubo;

layout(location = 0) out vec2 fragTexCoord;

void main() {
    // Apply projection to the vertex position
    gl_Position = ubo.projection * vec4(inPosition, 0.0, 1.0);
    fragTexCoord = inTexCoord;
}