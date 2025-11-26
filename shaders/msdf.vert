#version 450
layout(location=0) in vec2 inPos;
layout(location=1) in vec2 inUV;
layout(location=2) in vec4 inColor;

layout(location=0) out vec2 uv;
layout(location=1) out vec4 color;

void main() {
    uv = inUV;
    color = inColor;

    // assume your renderer expects framebuffer coords → NDC
    vec2 ndc = rendererToNDC(inPos);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
