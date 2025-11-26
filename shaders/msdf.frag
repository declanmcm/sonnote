#version 450

layout(binding = 0) uniform sampler2D msdf;

layout(location=0) in vec2 uv;
layout(location=1) in vec4 color;

layout(location=0) out vec4 outColor;

float median(float r, float g, float b) {
    return max(min(r,g), min(max(r,g), b));
}

void main() {
    vec3 sd = texture(msdf, uv).rgb;
    float d = median(sd.r, sd.g, sd.b);

    float w = fwidth(d);
    float alpha = smoothstep(0.5 - w, 0.5 + w, d);

    outColor = vec4(color.rgb, color.a * alpha);
}
