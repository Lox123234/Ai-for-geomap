#version 440
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec4 vColor;
layout(location = 3) in float vT;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 offset;
    vec4 flags;
    vec4 heights;
    vec4 light;
};
layout(binding = 1) uniform sampler2D tex;

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
    vec4 base = flags.w > 0.5 ? vColor : vec4(0.80, 0.82, 0.85, 1.0);
    if (flags.z > 0.5) {
        base = vec4(ramp(vT), 1.0);
    } else if (flags.x > 0.5) {
        vec4 t = texture(tex, vUv);
        base = vec4(mix(base.rgb, t.rgb, t.a), 1.0);
    }
    float shade = 1.0;
    if (flags.y > 0.5) {
        // Two-sided: meshes from photogrammetry do not have consistent windings.
        float d = abs(dot(normalize(vNormal), normalize(light.xyz)));
        shade = light.w + (1.0 - light.w) * d;
    }
    fragColor = vec4(base.rgb * shade, 1.0);
}