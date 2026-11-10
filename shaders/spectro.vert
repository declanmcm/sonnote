#version 450
layout(location=0) in vec2 inPos;
layout(location=1) in vec3 inColor; // unused

layout(location=0) out vec2 vUV;

layout(push_constant) uniform PC {
    vec4 uvRect; // (u0,v0,u1,v1)
} pc;

void main() {
    gl_Position = vec4(inPos, 0.0, 1.0);

    // inPos is NDC [-1,1] -> base UV [0,1]
    vec2 base = inPos * 0.5 + 0.5;
    // Map into uvRect
    vUV = mix(pc.uvRect.xy, pc.uvRect.zw, base);
}
