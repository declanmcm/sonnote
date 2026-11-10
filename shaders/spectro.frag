#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;

layout(set=0, binding=0) uniform sampler2D uTex;

void main() {
    float g = texture(uTex, vUV).r;

    // Simple dark spectrogram: adjust as you like
    float intensity = pow(g, 1.5); // contrast
    outColor = vec4(vec3(intensity), 1.0);
}
