#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColour;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1) uniform sampler2D atlas;
layout(push_constant) uniform PushConstants {
    float pixelRange; // This must be 4.0f
} pc;

// Helper function to find the median of three values
float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    // 1. Sample the multi-channel distance field texture
    vec3 msdf_sample = texture(atlas, fragUV).rgb;
    
    // 2. Reconstruct the single signed distance (d)
    // d is the normalized distance where 0.5 is the edge.
    float d = median(msdf_sample.r, msdf_sample.g, msdf_sample.b);
    
    // 3. Determine the distance in screen space pixels
    // The scale factor is calculated using derivatives (fwidth) to make the text scale smoothly
    // The range is the distance range used during atlas generation (pc.pixelRange)
    float screen_px_distance = d - 0.5;
    screen_px_distance *= pc.pixelRange;
    screen_px_distance /= length(vec2(dFdx(fragUV.x), dFdy(fragUV.y))) * pc.pixelRange;

    //float screen_px_distance = (d - 0.5) * pc.pixelRange;


    // 4. Calculate the Anti-aliasing/Smoothing Factor.
    // fwidth is a built-in GLSL function that calculates the absolute magnitude of the
    // derivative of 'dist' in both X and Y screen directions. This gives us the size of one pixel
    // in terms of the 'dist' units, which is perfect for smoothing.
    float smoothing = fwidth(screen_px_distance);

    // 5. Calculate the final alpha using smoothstep
    // smoothstep(min_edge, max_edge, value)
    // We want the edge to go from fully transparent (-smoothing) to fully opaque (+smoothing).
    float alpha = smoothstep(-smoothing, smoothing, screen_px_distance);

    // 6. Final Output
    outColor = vec4(fragColour.rgb, fragColour.a * alpha);
}