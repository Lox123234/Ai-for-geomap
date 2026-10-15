#version 440
// Point cloud: one screen-facing quad per point (instanced), size in pixels on every backend.
layout(location = 0) in vec2 corner;
layout(location = 1) in vec3 position;
layout(location = 2) in vec4 color;
layout(location = 3) in vec4 extra;   // x: ASPRS class / 255, y: intensity (0..1)
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vCorner;
layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 offset;     // xyz: layer origin in the scene; w: vertical scale
    vec4 params;     // xy: viewport in pixels; z: point size in pixels; w: colour mode (0 source, 1 height, 2 classes, 3 intensity)
    vec4 heights;    // x, y: scene height range; z: 1 = the cloud has colours
    vec4 data;       // x: 1 = the cloud has classes; y: 1 = it has intensity
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

// ASPRS classes, the same colours as view3d::classColor().
vec3 classColor(float value)
{
    int c = int(value * 255.0 + 0.5);
    if (c == 0) return vec3(160.0, 160.0, 160.0) / 255.0;
    if (c == 1) return vec3(190.0, 190.0, 190.0) / 255.0;
    if (c == 2) return vec3(166.0, 118.0, 60.0) / 255.0;
    if (c == 3) return vec3(166.0, 214.0, 110.0) / 255.0;
    if (c == 4) return vec3(80.0, 175.0, 70.0) / 255.0;
    if (c == 5) return vec3(34.0, 120.0, 50.0) / 255.0;
    if (c == 6) return vec3(225.0, 90.0, 70.0) / 255.0;
    if (c == 7 || c == 18) return vec3(1.0, 0.0, 1.0);
    if (c == 9) return vec3(60.0, 130.0, 230.0) / 255.0;
    if (c == 17) return vec3(230.0, 200.0, 90.0) / 255.0;
    return vec3(200.0, 200.0, 200.0) / 255.0;
}

void main()
{
    vec3 p = position + offset.xyz;
    vec4 clip = mvp * vec4(p, 1.0);
    clip.xy += corner * params.z / params.xy * clip.w;
    gl_Position = clip;
    float t = clamp((p.z - heights.x) / max(heights.y - heights.x, 0.001), 0.0, 1.0);
    int mode = int(params.w + 0.5);
    // A mode the cloud has no data for falls back to its colours, then to heights.
    if (mode == 2 && data.x > 0.5) vColor = vec4(classColor(extra.x), 1.0);
    else if (mode == 3 && data.y > 0.5) vColor = vec4(vec3(0.08 + 0.92 * extra.y), 1.0);
    else if (mode == 1 || heights.z < 0.5) vColor = vec4(ramp(t), 1.0);
    else vColor = color;
    vCorner = corner;
}
