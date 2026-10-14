#version 440
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vCorner;
layout(location = 0) out vec4 fragColor;
void main()
{
    if (dot(vCorner, vCorner) > 1.0) discard;   // round points
    fragColor = vColor;
}