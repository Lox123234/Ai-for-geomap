#version 440
// Point cloud: one screen-facing quad per point (instanced), size in pixels on every backend.
layout(location = 0) in vec2 corner;
layout(location = 1) in vec3 position;
layout(location = 2) in vec4 color;
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vCorner;
layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 offset;     // xyz: layer origin in the scene; w: vertical scale
    vec4 params;     // xy: viewport in pixels; z: point size in pixels; w: 1 = colour by height
    vec4 heights;    // x, y: scene height range; z: 1 = the cloud has colours
};

vec3 ramp(float t)
{
    // Elevation colours: low blue-green, middle yellow, high brown, top white.
    vec3 c0 = vec3(0.17, 0.42, 0.75);
    vec3 c1 = vec3(0.30, 0.68, 0.36);
    vec3 c2 = vec3(0.93, 0.85, 0.42);
    vec3 c3 = vec3(0.62, 0.43, 0.28);
    vec3 c4 = vec3(0.97, 0.97, 0.97);
    if (t < 0.25) return mix(c0, c1, t / 0.25);
    if (t < 0.5) return mix(c1, c2, (t - 0.25) / 0.25);
    if (t < 0.75) return mix(c2, c3, (t - 0.5) / 0.25);
    return mix(c3, c4, (t - 0.75) / 0.25);
}

void main()
{
    vec3 p = position + offset.xyz;
    vec4 clip = mvp * vec4(p, 1.0);
    clip.xy += corner * params.z / params.xy * clip.w;
    gl_Position = clip;
    float t = clamp((p.z - heights.x) / max(heights.y - heights.x, 0.001), 0.0, 1.0);
    vColor = (params.w > 0.5 || heights.z < 0.5) ? vec4(ramp(t), 1.0) : color;
    vCorner = corner;
}