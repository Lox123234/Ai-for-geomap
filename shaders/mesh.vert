#version 440
// Meshes and terrains: texture, vertex colours or colour by height; simple sun lighting.
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
layout(location = 3) in vec4 color;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUv;
layout(location = 2) out vec4 vColor;
layout(location = 3) out float vT;
layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 offset;     // xyz: layer origin in the scene; w: vertical scale
    vec4 flags;      // x: texture; y: lighting; z: colour by height; w: vertex colours
    vec4 heights;    // x, y: scene height range
    vec4 light;      // xyz: direction to the sun; w: ambient
};
void main()
{
    vec3 p = position + offset.xyz;
    gl_Position = mvp * vec4(p, 1.0);
    // Normals of the vertically stretched surface.
    vNormal = normalize(vec3(normal.xy * max(offset.w, 0.01), normal.z));
    vUv = uv;
    vColor = color;
    vT = clamp((p.z - heights.x) / max(heights.y - heights.x, 0.001), 0.0, 1.0);
}